#include "components/tooltip.hpp"

#include "common/test.hpp"
#include "components/config.hpp"
#include "tags/action_context.hpp"

using namespace polybar;

TEST(TooltipOptions, DefaultsAndBarColors) {
  logger log{loglevel::NONE};
  config conf{log, "test", "test"};
  conf.set_sections({{"module/test", {}}});
  bar_settings bar;
  bar.background = rgba{0xff123456};
  tooltip_options opts{conf, "module/test", bar};
  EXPECT_EQ(350, opts.delay);
  EXPECT_EQ(2000, opts.timeout);
  EXPECT_EQ(400, opts.max_width);
  EXPECT_EQ(300, opts.max_height);
  EXPECT_EQ(8, opts.offset);
  EXPECT_EQ(10, opts.padding);
  EXPECT_EQ(1, opts.border);
  EXPECT_EQ(bar.background, opts.background);
  EXPECT_EQ(bar.foreground, opts.foreground);
  EXPECT_EQ(bar.foreground, opts.border_color);
  EXPECT_FALSE(opts.dynamic);
}

TEST(TooltipOptions, ContentPrecedenceAndValidation) {
  logger log{loglevel::NONE};
  config conf{log, "test", "test"};
  conf.set_sections(
      {{"module/test", {{"tooltip", "Hello\\nworld"}, {"tooltip-exec", ""}, {"tooltip-delay", "0"},
                           {"tooltip-exec-timeout", "-1"}, {"tooltip-max-width", "0"}, {"tooltip-padding", "invalid"},
                           {"tooltip-border-size", "0"}, {"tooltip-background", "#abcdef"}}}});
  tooltip_options opts{conf, "module/test", bar_settings{}};
  EXPECT_EQ("Hello\nworld", opts.text);
  EXPECT_TRUE(opts.dynamic); // Even an explicitly empty command overrides static text.
  EXPECT_EQ(0, opts.delay);
  EXPECT_EQ(2000, opts.timeout);
  EXPECT_EQ(400, opts.max_width);
  EXPECT_EQ(10, opts.padding);
  EXPECT_EQ(0, opts.border);
  EXPECT_EQ(rgba{"#abcdef"}, opts.background);
}

TEST(TooltipPosition, NonPrimaryMonitorAndEdges) {
  xcb_rectangle_t monitor{1920, 100, 1280, 900};
  xcb_rectangle_t bar{1950, 110, 1200, 30};
  auto rect = tooltip_position(monitor, bar, 0, 20, 400, 200, 8, false);
  EXPECT_EQ(1920, rect.x);
  EXPECT_EQ(148, rect.y);
  rect = tooltip_position(monitor, bar, 1180, 1200, 400, 200, 8, false);
  EXPECT_EQ(2800, rect.x);
  bar.y = 950;
  rect = tooltip_position(monitor, bar, 100, 200, 400, 200, 8, true);
  EXPECT_EQ(742, rect.y);
}

TEST(TooltipPosition, NegativeOriginAndOversize) {
  auto rect = tooltip_position({-1280, -100, 1280, 720}, {-1280, 590, 1280, 30}, 500, 600, 2000, 2000, 8, true);
  EXPECT_EQ(-1280, rect.x);
  EXPECT_EQ(-100, rect.y);
  EXPECT_EQ(1280, rect.width);
  EXPECT_EQ(720, rect.height);
}

TEST(TooltipRegions, AlignmentTransitionsResetAndClickIndependence) {
  tags::action_context ctxt;
  ctxt.set_alignment_start(alignment::LEFT, 12);
  ctxt.set_alignment_start(alignment::RIGHT, 700);
  ctxt.tooltip_open(alignment::LEFT, 0);
  ctxt.action_open(mousebtn::LEFT, "click", alignment::LEFT, 10);
  ctxt.action_close(mousebtn::NONE, alignment::LEFT, 20);
  ctxt.tooltip_close(alignment::LEFT, 100);
  ctxt.tooltip_open(alignment::RIGHT, 0);
  ctxt.tooltip_close(alignment::RIGHT, 80);
  EXPECT_EQ(-1, ctxt.tooltip_at(11).first);
  EXPECT_EQ(0, ctxt.tooltip_at(12).first);
  EXPECT_EQ(0, ctxt.tooltip_at(111).first);
  EXPECT_EQ(-1, ctxt.tooltip_at(112).first);
  EXPECT_EQ(1, ctxt.tooltip_at(700).first);
  EXPECT_EQ(700, ctxt.tooltip_at(700).second.first);
  EXPECT_EQ(780, ctxt.tooltip_at(700).second.second);
  EXPECT_EQ(1u, ctxt.num_actions());
  EXPECT_EQ("click", ctxt.get_action(ctxt.has_action(mousebtn::LEFT, 25)));
  // A rerender replaces old module bounds, including when modules disappear.
  ctxt.reset();
  EXPECT_EQ(-1, ctxt.tooltip_at(25).first);
}

TEST(TooltipRegions, NegativeOffsetsAndResizing) {
  tags::action_context ctxt;
  ctxt.tooltip_open(alignment::CENTER, 20);
  ctxt.compensate_for_negative_move(alignment::CENTER, 50, 10);
  ctxt.tooltip_close(alignment::CENTER, 30);
  ctxt.set_alignment_start(alignment::CENTER, 100);
  EXPECT_EQ(0, ctxt.tooltip_at(110).first);
  EXPECT_EQ(0, ctxt.tooltip_at(149).first);
  EXPECT_EQ(-1, ctxt.tooltip_at(150).first);
  ctxt.set_alignment_start(alignment::CENTER, 200);
  EXPECT_EQ(-1, ctxt.tooltip_at(110).first);
  EXPECT_EQ(0, ctxt.tooltip_at(210).first);
}
