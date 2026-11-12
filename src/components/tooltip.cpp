#include "components/tooltip.hpp"

#include <cairo/cairo-xcb.h>
#include <signal.h>
#include <xcb/shape.h>

#include <algorithm>
#include <cmath>

#include "cairo/font.hpp"
#include "components/config.hpp"
#include "utils/env.hpp"
#include "utils/string.hpp"
#include "x11/connection.hpp"
#include "x11/extensions/randr.hpp"

POLYBAR_NS

tooltip_options::tooltip_options(const config& conf, const string& section, const bar_settings& bar) {
  text = string_util::replace_all(conf.get(section, "tooltip", ""s), "\\n", "\n");
  dynamic = conf.has(section, "tooltip-exec");
  command = conf.get(section, "tooltip-exec", ""s);
  const auto number = [&](const string& key, int fallback, int minimum, int maximum) {
    try {
      string raw = conf.get(section, key, to_string(fallback));
      size_t consumed;
      long long value = std::stoll(raw, &consumed);
      if (consumed != raw.size() || value < minimum || value > maximum)
        throw std::out_of_range(key);
      return int(value);
    } catch (const std::exception& err) {
      logger::make().err("Invalid value for %s.%s, using default %d (reason: %s)", section, key, fallback, err.what());
      return fallback;
    }
  };
  delay = number("tooltip-delay", delay, 0, 3600000);
  timeout = number("tooltip-exec-timeout", timeout, 1, 3600000);
  max_width = number("tooltip-max-width", max_width, 1, 32767);
  max_height = number("tooltip-max-height", max_height, 1, 32767);
  offset = number("tooltip-offset", offset, 0, 32767);
  padding = number("tooltip-padding", padding, 0, 32767);
  border = number("tooltip-border-size", border, 0, 32767);
  background = conf.get(section, "tooltip-background", bar.background);
  foreground = conf.get(section, "tooltip-foreground", bar.foreground);
  border_color = conf.get(section, "tooltip-border-color", bar.foreground);
}

xcb_rectangle_t tooltip_position(xcb_rectangle_t area, xcb_rectangle_t bar, double start, double end, int width,
    int height, int offset, bool bottom) {
  width = std::max(1, std::min(width, int(area.width)));
  height = std::max(1, std::min(height, int(area.height)));
  int x = bar.x + std::lround((start + end - width) / 2);
  int y = bottom ? bar.y - offset - height : bar.y + bar.height + offset;
  x = std::max(int(area.x), std::min(x, area.x + area.width - width));
  y = std::max(int(area.y), std::min(y, area.y + area.height - height));
  return {int16_t(x), int16_t(y), uint16_t(width), uint16_t(height)};
}

struct tooltip::job {
  uv_process_t process{};
  eventloop::pipe_handle_t pipe;
  tooltip* owner{nullptr};
  string output;
  bool exited{false}, eof{false};
  int64_t status{-1};
  int signal{0};
};

tooltip::tooltip(
    connection& conn, eventloop::loop& loop, const config& conf, const logger& log, const bar_settings& bar)
    : m_connection(conn)
    , m_loop(loop)
    , m_conf(conf)
    , m_log(log)
    , m_bar(bar)
    , m_timer(loop.handle<eventloop::TimerHandle>()) {}

tooltip::~tooltip() {
  leave();
  m_timer->close();
}

void tooltip::configure(const vector<string>& names) {
  for (const auto& name : names) {
    if (!m_options.count(name))
      m_options.emplace(name, tooltip_options(m_conf, name, m_bar));
  }
  if (!m_active.empty() && std::find(names.begin(), names.end(), m_active) == names.end())
    leave();
  m_names = names;
}

void tooltip::hover(int index, double start, double end) {
  if (index < 0 || size_t(index) >= m_names.size() || end <= start) {
    leave();
    return;
  }
  string name = m_names[index];
  bool moved = m_start != start || m_end != end;
  m_start = start;
  m_end = end;
  if (name == m_active) {
    if (moved && m_window != XCB_NONE)
      draw();
    return;
  }
  leave();
  m_active = name;
  m_style = m_options.at(name);
  m_timer->start(m_style.delay, 0, [this]() { display(); });
}

void tooltip::leave() {
  m_timer->stop();
  if (m_job) {
    m_job->owner = nullptr;
    // Detached libuv children form a process group. Kill descendants as well as the shell,
    // including commands that ignore SIGTERM. libuv reaps the shell in its exit callback.
    kill(-m_job->process.pid, SIGKILL);
    m_job->pipe->close();
    m_job.reset();
  }
  if (m_window != XCB_NONE) {
    xcb_destroy_window(m_connection, m_window);
    m_window = XCB_NONE;
    m_connection.flush();
  }
  m_active.clear();
  m_text.clear();
}

void tooltip::display() {
  m_log.trace("tooltip: Displaying %s", m_active);
  try {
    if (m_style.dynamic) {
      execute();
    } else {
      m_text = m_style.text.substr(0, 65536);
      draw();
    }
  } catch (const std::exception& err) {
    m_log.err("%s: Cannot start tooltip: %s", m_active, err.what());
    string active = m_active;
    leave();
    m_active = active;
  }
}

void tooltip::execute() {
  if (m_style.command.empty())
    return;
  auto task = std::make_shared<job>();
  task->owner = this;
  task->pipe = m_loop.handle<eventloop::PipeHandle>();
  string shell = env_util::get("POLYBAR_SHELL", "/bin/sh");
  char* args[] = {
      const_cast<char*>(shell.c_str()), const_cast<char*>("-c"), const_cast<char*>(m_style.command.c_str()), nullptr};
  uv_stdio_container_t stdio[3]{};
  stdio[0].flags = UV_IGNORE;
  stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
  stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(task->pipe->raw());
  // Preserve stderr for diagnostics; only stdout is tooltip content.
  stdio[2].flags = UV_INHERIT_FD;
  stdio[2].data.fd = 2;
  uv_process_options_t opts{};
  opts.file = shell.c_str();
  opts.args = args;
  opts.flags = UV_PROCESS_DETACHED;
  opts.stdio_count = 3;
  opts.stdio = stdio;
  opts.exit_cb = [](uv_process_t* process, int64_t status, int signal) {
    auto task = *static_cast<shared_ptr<job>*>(process->data);
    task->exited = true;
    task->status = status;
    task->signal = signal;
    if (task->owner)
      task->owner->complete(task);
    uv_close(reinterpret_cast<uv_handle_t*>(process),
        [](uv_handle_t* handle) { delete static_cast<shared_ptr<job>*>(handle->data); });
  };
  task->process.data = new shared_ptr<job>(task);
  int result = uv_spawn(m_loop.get(), &task->process, &opts);
  if (result < 0) {
    m_log.err("%s: tooltip-exec failed: %s", m_active, uv_strerror(result));
    task->pipe->close();
    uv_close(reinterpret_cast<uv_handle_t*>(&task->process),
        [](uv_handle_t* handle) { delete static_cast<shared_ptr<job>*>(handle->data); });
    return;
  }
  m_job = task;
  m_timer->start(m_style.timeout, 0, [this]() {
    m_log.warn("%s: tooltip-exec timed out after %d ms", m_active, m_style.timeout);
    string active = m_active;
    leave();
    m_active = active; // A failed entry must not start again on pointer motion.
  });
  task->pipe->read_start(
      [task](const eventloop::ReadEvent& event) {
        if (!task->owner)
          return;
        if (task->output.size() + event.len > 65536) {
          auto* owner = task->owner;
          owner->m_log.warn("%s: tooltip-exec exceeded 64 KiB output limit", owner->m_active);
          string active = owner->m_active;
          owner->leave();
          owner->m_active = active;
          return;
        }
        task->output.append(event.data, event.len);
      },
      [task]() {
        task->eof = true;
        if (task->owner)
          task->owner->complete(task);
      },
      [task](const eventloop::ErrorEvent&) {
        if (task->owner) {
          auto* owner = task->owner;
          owner->m_log.warn("%s: Cannot read tooltip-exec output", owner->m_active);
          string active = owner->m_active;
          owner->leave();
          owner->m_active = active;
        }
      });
}

void tooltip::complete(const shared_ptr<job>& task) {
  if (!task->exited || (!task->eof && task->status == 0 && task->signal == 0))
    return;
  m_timer->stop();
  task->owner = nullptr;
  kill(-task->process.pid, SIGKILL);
  task->pipe->close();
  m_job.reset();
  if (task->status != 0 || task->signal != 0) {
    m_log.warn("%s: tooltip-exec failed (exit=%lld, signal=%d)", m_active, (long long)task->status, task->signal);
    return;
  }
  m_text = task->output;
  draw();
}

xcb_rectangle_t tooltip::workarea() const {
  auto mon = m_bar.monitor;
  xcb_rectangle_t area{mon->x, mon->y, mon->w, mon->h};
  // EWMH workarea is global. Intersect it with this monitor rather than assuming
  // either a primary monitor or a root origin of (0, 0).
  const auto atom = [&](const char* name) {
    auto reply = m_connection.intern_atom(false, strlen(name), name);
    return reply->atom;
  };
  auto desktop =
      m_connection.get_property(false, m_connection.root(), atom("_NET_CURRENT_DESKTOP"), XCB_ATOM_CARDINAL, 0, 1);
  uint32_t index = desktop.get() && desktop->value_len == 1 ? *desktop.value<uint32_t>().begin() : 0;
  auto work =
      m_connection.get_property(false, m_connection.root(), atom("_NET_WORKAREA"), XCB_ATOM_CARDINAL, index * 4, 4);
  if (work.get() && work->value_len == 4) {
    auto values = work.value<uint32_t>();
    auto it = values.begin();
    int x = *it++, y = *it++, w = *it++, h = *it;
    int left = std::max(int(area.x), x), top = std::max(int(area.y), y);
    int right = std::min(int(area.x + area.width), x + w);
    int bottom = std::min(int(area.y + area.height), y + h);
    if (right > left && bottom > top)
      area = {int16_t(left), int16_t(top), uint16_t(right - left), uint16_t(bottom - top)};
  }
  return area;
}

void tooltip::draw() {
  try {
    render();
  } catch (const std::exception& err) {
    m_log.err("%s: Cannot render tooltip: %s", m_active, err.what());
    if (m_window != XCB_NONE) {
      xcb_destroy_window(m_connection, m_window);
      m_window = XCB_NONE;
      m_connection.flush();
    }
  }
}

void tooltip::render() {
  while (!m_text.empty() && (m_text.back() == '\n' || m_text.back() == '\r')) m_text.pop_back();
  if (m_text.empty() || m_active.empty())
    return;
  auto area = workarea();
  int max_width = std::min(m_style.max_width, int(area.width));
  int max_height = std::min(m_style.max_height, int(area.height));
  int inset = std::min(m_style.padding + m_style.border, std::max(0, (std::min(max_width, max_height) - 1) / 2));
  // Layout onto an image first; the same font infrastructure used by the bar
  // measures each UTF-8 codepoint, with fallback fonts, before wrapping.
  auto surface = cairo_image_surface_create(CAIRO_FORMAT_RGB24, max_width, max_height);
  scope_util::on_exit surface_guard([surface] { cairo_surface_destroy(surface); });
  auto cr = cairo_create(surface);
  scope_util::on_exit context_guard([cr] { cairo_destroy(cr); });
  vector<shared_ptr<cairo::font_fc>> fonts;
  auto patterns = m_conf.get_list<string>(m_conf.section(), "font", {"fixed"});
  for (auto pattern : patterns) {
    auto pos = pattern.rfind(';');
    if (pos != string::npos)
      pattern.erase(pos);
    fonts.push_back(cairo::make_font(cr, move(pattern), 0, m_bar.dpi_x, m_bar.dpi_y));
  }
  if (fonts.empty())
    fonts.push_back(cairo::make_font(cr, "fixed", 0, m_bar.dpi_x, m_bar.dpi_y));
  double line_height = 1, ascent = 1;
  for (auto& font : fonts) {
    line_height = std::max(line_height, std::ceil(font->extents().height));
    ascent = std::max(ascent, std::ceil(font->extents().ascent));
  }
  struct glyph {
    string text;
    shared_ptr<cairo::font_fc> font;
    double x, y;
  };
  vector<glyph> glyphs;
  string utf8 = m_text;
  string_util::unicode_charlist chars;
  if (!string_util::utf8_to_ucs4(utf8, chars))
    m_log.warn("%s: Dropping invalid UTF-8 in tooltip", m_active);
  double x = inset, y = inset, used = 0;
  double ink_top = 0, ink_bottom = 0;
  bool has_ink = false;
  for (auto ch : chars) {
    if (ch.codepoint == '\r')
      continue;
    if (ch.codepoint == '\n') {
      used = std::max(used, x - inset);
      x = inset;
      y += line_height;
      continue;
    }
    if (ch.codepoint < 32 && ch.codepoint != '\t')
      continue;
    string text = ch.codepoint == '\t' ? "    " : utf8.substr(ch.offset, ch.length);
    auto font = fonts.front();
    for (auto& candidate : fonts)
      if (candidate->match(ch)) {
        font = candidate;
        break;
      }
    cairo_text_extents_t ext{};
    font->textwidth(text, &ext);
    double advance = std::ceil(ext.x_advance);
    if (x > inset && x + advance > max_width - inset) {
      used = std::max(used, x - inset);
      x = inset;
      y += line_height;
    }
    if (y + line_height > max_height - inset)
      break;
    glyphs.push_back({text, font, x, y + ascent});
    if (ext.height > 0) {
      double top = y + ascent + ext.y_bearing;
      double bottom = top + ext.height;
      ink_top = has_ink ? std::min(ink_top, top) : top;
      ink_bottom = has_ink ? std::max(ink_bottom, bottom) : bottom;
      has_ink = true;
    }
    x += advance;
    used = std::max(used, x - inset);
  }
  int width = std::min(max_width, std::max(1, int(std::ceil(used)) + 2 * inset));
  int height = std::min(max_height, std::max(1, int(std::ceil(y + line_height)) + inset));
  if (has_ink) {
    // Font ascent/descent includes unequal whitespace above and below the ink.
    // Pad the visible text bounds instead, retaining the baseline spacing between lines.
    double top = std::floor(ink_top);
    for (auto& glyph : glyphs) {
      glyph.y += inset - top;
    }
    height = std::min(max_height, int(std::ceil(ink_bottom) - top) + 2 * inset);
  }
  const auto color = [&](rgba value) { cairo_set_source_rgb(cr, value.red_d(), value.green_d(), value.blue_d()); };
  color(m_style.border_color);
  cairo_paint(cr);
  int border = std::min(m_style.border, std::min(width, height) / 2);
  color(m_style.background);
  cairo_rectangle(cr, border, border, width - 2 * border, height - 2 * border);
  cairo_fill(cr);
  cairo_rectangle(cr, inset, inset, std::max(0, width - 2 * inset), std::max(0, height - 2 * inset));
  cairo_clip(cr);
  color(m_style.foreground);
  for (auto& glyph : glyphs) {
    glyph.font->use();
    glyph.font->render(glyph.text, glyph.x, glyph.y);
  }
  fonts.clear();
  auto translated = m_connection.translate_coordinates(m_bar.x_data.window, m_connection.root(), 0, 0);
  xcb_rectangle_t bar{translated->dst_x, translated->dst_y, uint16_t(m_bar.size.w), uint16_t(m_bar.size.h)};
  auto rect = tooltip_position(area, bar, m_start, m_end, width, height, m_style.offset, m_bar.bottom);
  auto screen = m_connection.screen();
  if (m_window == XCB_NONE) {
    m_window = m_connection.generate_id();
    // Override redirect avoids tiling, taskbar entries, focus, and struts. An
    // empty Shape input region makes the popup transparent to pointer input.
    uint32_t attributes[] = {1};
    xcb_create_window(m_connection, screen->root_depth, m_window, screen->root, rect.x, rect.y, rect.width, rect.height,
        0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual, XCB_CW_OVERRIDE_REDIRECT, attributes);
    uint32_t hints[9] = {1, 0}; // ICCCM InputHint: never accept keyboard focus.
    xcb_change_property(
        m_connection, XCB_PROP_MODE_REPLACE, m_window, XCB_ATOM_WM_HINTS, XCB_ATOM_WM_HINTS, 32, 9, hints);
    const string name = "polybar-tooltip";
    xcb_change_property(
        m_connection, XCB_PROP_MODE_REPLACE, m_window, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, name.size(), name.data());
    auto type_atom = m_connection.intern_atom(false, strlen("_NET_WM_WINDOW_TYPE"), "_NET_WM_WINDOW_TYPE");
    auto tooltip_atom =
        m_connection.intern_atom(false, strlen("_NET_WM_WINDOW_TYPE_TOOLTIP"), "_NET_WM_WINDOW_TYPE_TOOLTIP");
    xcb_atom_t type = tooltip_atom->atom;
    xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_window, type_atom->atom, XCB_ATOM_ATOM, 32, 1, &type);
    xcb_shape_rectangles(
        m_connection, XCB_SHAPE_SO_SET, XCB_SHAPE_SK_INPUT, XCB_CLIP_ORDERING_UNSORTED, m_window, 0, 0, 0, nullptr);
  }
  uint32_t geometry[] = {
      uint32_t(int32_t(rect.x)), uint32_t(int32_t(rect.y)), rect.width, rect.height, XCB_STACK_MODE_ABOVE};
  xcb_configure_window(m_connection, m_window,
      XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
          XCB_CONFIG_WINDOW_STACK_MODE,
      geometry);
  // A background pixmap preserves pixels across expose events without another event sink.
  auto pixmap = m_connection.generate_id();
  xcb_create_pixmap(m_connection, screen->root_depth, pixmap, m_window, rect.width, rect.height);
  auto target = cairo_xcb_surface_create(
      m_connection, pixmap, m_connection.visual_type_for_id(screen->root_visual), rect.width, rect.height);
  auto target_cr = cairo_create(target);
  cairo_set_source_surface(target_cr, surface, 0, 0);
  cairo_paint(target_cr);
  cairo_destroy(target_cr);
  cairo_surface_destroy(target);
  xcb_change_window_attributes(m_connection, m_window, XCB_CW_BACK_PIXMAP, &pixmap);
  xcb_free_pixmap(m_connection, pixmap);
  xcb_clear_area(m_connection, false, m_window, 0, 0, 0, 0);
  xcb_map_window(m_connection, m_window);
  m_connection.flush();
}
POLYBAR_NS_END
