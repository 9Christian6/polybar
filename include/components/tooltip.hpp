#pragma once

#include <map>

#include "common.hpp"
#include "components/eventloop.hpp"
#include "components/types.hpp"

POLYBAR_NS
class config;
class connection;
class logger;

struct tooltip_options {
  tooltip_options() = default;
  tooltip_options(const config&, const string& section, const bar_settings&);
  string text, command;
  bool dynamic{false};
  int delay{350}, timeout{2000}, max_width{400}, max_height{300}, offset{8}, padding{10}, border{1};
  rgba background{0xff000000}, foreground{0xffffffff}, border_color{0xffffffff};
};

// All coordinates are root coordinates, including monitors with nonzero origins.
xcb_rectangle_t tooltip_position(xcb_rectangle_t area, xcb_rectangle_t bar, double start, double end, int width,
    int height, int offset, bool bottom);

class tooltip {
 public:
  tooltip(connection&, eventloop::loop&, const config&, const logger&, const bar_settings&);
  ~tooltip();
  void configure(const vector<string>& names);
  void hover(int index, double start, double end);
  void leave();
  bool enabled() const {
    return !m_names.empty();
  }

 private:
  struct job;
  void display();
  void execute();
  void complete(const shared_ptr<job>&);
  void draw();
  void render();
  xcb_rectangle_t workarea() const;
  connection& m_connection;
  eventloop::loop& m_loop;
  const config& m_conf;
  const logger& m_log;
  const bar_settings& m_bar;
  eventloop::timer_handle_t m_timer;
  vector<string> m_names;
  std::map<string, tooltip_options> m_options;
  string m_active, m_text;
  tooltip_options m_style;
  double m_start{0}, m_end{0};
  xcb_window_t m_window{XCB_NONE};
  shared_ptr<job> m_job;
};
POLYBAR_NS_END
