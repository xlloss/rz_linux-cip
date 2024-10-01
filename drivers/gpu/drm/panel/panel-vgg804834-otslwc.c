// SPDX-License-Identifier: GPL-2.0
/*
 * EVERVISION VGG804834-0TSLWC MIPI-DSI panel driver
 *
 * slash.huang@regulus.com.tw
 * slash.linux.c@gmail.com
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#define H_PRO_OFF 30
#define H_MULTI 4

static const u32 otslw_bus_formats[] = {
	MEDIA_BUS_FMT_RGB888_1X24,
};

struct otslw_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;

	struct gpio_desc *reset;
	struct gpio_desc *bl_pwr;
	struct backlight_device *backlight;

	struct regulator_bulk_data *supplies;
	unsigned int num_supplies;

	bool prepared;
	bool enabled;
};

static const struct drm_display_mode default_mode = {
	.clock = 30000 * H_MULTI,
	.hdisplay = 800,
	.hsync_start = 800 * H_MULTI + (40 + H_PRO_OFF),
	.hsync_end = 800 * H_MULTI + (40 + H_PRO_OFF) + (48 - H_PRO_OFF),
	.htotal = 800 * H_MULTI + (40 + H_PRO_OFF) + (48 - H_PRO_OFF) + 40,

	.vdisplay = 480,
	.vsync_start = 480 + (13),
	.vsync_end = 480 + (13) + (3),
	.vtotal = 480 + (13) + (3) + 29,

	.width_mm = 200,
	.height_mm = 0,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static inline struct otslw_panel *to_otslw_panel(struct drm_panel *panel)
{
	return container_of(panel, struct otslw_panel, panel);
}

static int otslw_panel_prepare(struct drm_panel *panel)
{
	struct otslw_panel *otslw = to_otslw_panel(panel);

	if (otslw->prepared)
		return 0;

	if (otslw->reset) {
		gpiod_set_value_cansleep(otslw->reset, 1);
		usleep_range(3000, 5000);
		gpiod_set_value_cansleep(otslw->reset, 0);
		usleep_range(18000, 20000);
		gpiod_set_value_cansleep(otslw->reset, 1);
	}

	otslw->prepared = true;

	return 0;
}

static int otslw_panel_unprepare(struct drm_panel *panel)
{
	struct otslw_panel *otslw = to_otslw_panel(panel);

	if (!otslw->prepared)
		return 0;

	otslw->prepared = false;

	return 0;
}

static int otslw_panel_enable(struct drm_panel *panel)
{
	struct otslw_panel *otslw = to_otslw_panel(panel);

	otslw->enabled = true;
	backlight_disable(otslw->backlight);

	return 0;
}

static int otslw_panel_disable(struct drm_panel *panel)
{
	struct otslw_panel *otslw = to_otslw_panel(panel);
	struct mipi_dsi_device *dsi = otslw->dsi;

	if (!otslw->enabled)
		return 0;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	backlight_disable(otslw->backlight);
	otslw->enabled = false;

	return 0;
}

static int otslw_panel_get_modes(struct drm_panel *panel,
			       struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(panel->dev, "failed to add mode %ux%u@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}
	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	connector->display_info.bus_flags = mode->flags;

	drm_display_info_set_bus_formats(&connector->display_info,
					 otslw_bus_formats,
					 ARRAY_SIZE(otslw_bus_formats));
	return 1;
}

static int otslw_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct otslw_panel *otslw = mipi_dsi_get_drvdata(dsi);

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	bl->props.brightness =
		gpiod_get_value_cansleep(otslw->bl_pwr) == 1 ? 255 : 0;

	return bl->props.brightness & 0xff;
}

static int otslw_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct otslw_panel *otslw = mipi_dsi_get_drvdata(dsi);

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	gpiod_set_value_cansleep(otslw->bl_pwr, bl->props.brightness & 1);

	return 0;
}

static const struct backlight_ops otslw_bl_ops = {
	.update_status = otslw_bl_update_status,
	.get_brightness = otslw_bl_get_brightness,
};

static const struct drm_panel_funcs otslw_panel_funcs = {
	.prepare = otslw_panel_prepare,
	.unprepare = otslw_panel_unprepare,
	.enable = otslw_panel_enable,
	.disable = otslw_panel_disable,
	.get_modes = otslw_panel_get_modes,
};

static const char * const otslw_supply_names[] = {
	"v5p0",
};

static int otslw_init_regulators(struct otslw_panel *otslw)
{
	struct device *dev = &otslw->dsi->dev;
	int i;

	otslw->num_supplies = ARRAY_SIZE(otslw_supply_names);
	otslw->supplies = devm_kcalloc(dev, otslw->num_supplies,
				     sizeof(*otslw->supplies), GFP_KERNEL);
	if (!otslw->supplies)
		return -ENOMEM;

	for (i = 0; i < otslw->num_supplies; i++)
		otslw->supplies[i].supply = otslw_supply_names[i];

	return devm_regulator_bulk_get(dev, otslw->num_supplies, otslw->supplies);
};

static int otslw_panel_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct device_node *np = dev->of_node;
	struct otslw_panel *panel;
	struct backlight_properties bl_props;
	u32 video_mode;
	int ret;

	panel = devm_kzalloc(&dsi->dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	mipi_dsi_set_drvdata(dsi, panel);
	panel->dsi = dsi;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;

	ret = of_property_read_u32(np, "video-mode", &video_mode);
	if (!ret) {
		switch (video_mode) {
		case 0:
			/* burst mode */
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_BURST;
			break;
		case 1:
			/* non-burst mode with sync event */
			break;
		case 2:
			/* non-burst mode with sync pulse */
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
			break;
		default:
			dev_warn(dev, "invalid video mode %d\n", video_mode);
			break;
		}
	}

	ret = of_property_read_u32(np, "dsi-lanes", &dsi->lanes);
	if (ret) {
		dev_err(dev, "Failed to get dsi-lanes property (%d)\n", ret);
		return ret;
	}

	panel->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(panel->reset))
		return PTR_ERR(panel->reset);

	panel->bl_pwr = devm_gpiod_get_optional(dev, "blpwr", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->bl_pwr))
		return PTR_ERR(panel->bl_pwr);

	memset(&bl_props, 0, sizeof(bl_props));
	bl_props.type = BACKLIGHT_RAW;
	bl_props.brightness = 0;
	bl_props.max_brightness = 255;

	panel->backlight = devm_backlight_device_register(dev, dev_name(dev),
							  dev, dsi, &otslw_bl_ops,
							  &bl_props);
	if (IS_ERR(panel->backlight)) {
		ret = PTR_ERR(panel->backlight);
		dev_err(dev, "Failed to register backlight (%d)\n", ret);
		return ret;
	}

	ret = otslw_init_regulators(panel);
	if (ret)
		return ret;

	drm_panel_init(&panel->panel, dev, &otslw_panel_funcs,
		       DRM_MODE_CONNECTOR_DSI);

	dev_set_drvdata(dev, panel);
	drm_panel_add(&panel->panel);
	ret = mipi_dsi_attach(dsi);
	if (ret)
		drm_panel_remove(&panel->panel);

	gpiod_set_value_cansleep(panel->reset, 1);
	backlight_enable(panel->backlight);

	return ret;
}

static int otslw_panel_remove(struct mipi_dsi_device *dsi)
{
	struct otslw_panel *otslw = mipi_dsi_get_drvdata(dsi);
	struct device *dev = &dsi->dev;
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret)
		dev_err(dev, "Failed to detach from host (%d)\n", ret);

	drm_panel_remove(&otslw->panel);

	return 0;
}

static void otslw_panel_shutdown(struct mipi_dsi_device *dsi)
{
	struct otslw_panel *otslw = mipi_dsi_get_drvdata(dsi);

	otslw_panel_disable(&otslw->panel);
	otslw_panel_unprepare(&otslw->panel);
}

static const struct of_device_id otslw_of_match[] = {
	{ .compatible = "evervision,vgg804834-otslwc", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, otslw_of_match);

static struct mipi_dsi_driver otslw_panel_driver = {
	.driver = {
		.name = "panel-vgg804834-otslwc",
		.of_match_table = otslw_of_match,
	},
	.probe = otslw_panel_probe,
	.remove = otslw_panel_remove,
	.shutdown = otslw_panel_shutdown,
};
module_mipi_dsi_driver(otslw_panel_driver);

MODULE_AUTHOR("Slash Huang <slash.linux.c@gmail.com>");
MODULE_DESCRIPTION("DRM Driver for VGG804834-0TSLWC MIPI DSI PANEL");
MODULE_LICENSE("GPL v2");
