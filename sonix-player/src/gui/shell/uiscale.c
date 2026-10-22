#include "uiscale.h"

#include "src/gui/shell/icons.h"

static int ui_panel_width = UI_DESIGN_WIDTH;

void ui_scale_init(int panel_width) {
	if (panel_width > 0) {
		ui_panel_width = panel_width;
	}
	// The icons are bitmaps, drawn for a few widths ahead of time (icons.h).
	icons_follow_panel(ui_panel_width);
}

int32_t ui_px(int32_t px) {
	if (ui_panel_width == UI_DESIGN_WIDTH) {
		return px;
	}
	int32_t magnitude = ((px < 0 ? -px : px) * ui_panel_width + UI_DESIGN_WIDTH / 2) / UI_DESIGN_WIDTH;
	return px < 0 ? -magnitude : magnitude;
}
