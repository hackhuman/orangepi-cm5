#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_graph.h>
#include <linux/pm_runtime.h>
#include <linux/rk-camera-module.h>
#include <media/media-entity.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define VERSION_MAJOR 1
#define VERSION_MINOR 1

#define GMAX4002_NAME "gmax4002"

#define GMAX4002_WIDTH 2048
#define GMAX4002_HEIGHT 1218
#define GMAX4002_DUMMY_ROWS 18
#define GMAX4002_ACTIVE_HEIGHT (GMAX4002_HEIGHT - GMAX4002_DUMMY_ROWS)

static const struct v4l2_fract GMAX4002_MAX_FPS = { .numerator = 1,
													.denominator = 166 };
#define GMAX4002_FRAME_INTERVAL GMAX4002_MAX_FPS
#define GMAX4002_FRAME_CODE MEDIA_BUS_FMT_Y10_1X10

#define GMAX4002_LINK_FREQ 600000000ULL
#define GMAX4002_BPP 10
#define GMAX4002_LANES 4

#define GMAX4002_VBLANK_MIN 28
#define GMAX4002_VBLANK_MAX 5450
#define GMAX4002_HBLANK 352

#define GMAX4002_CHIP_ID 0x0FA2

#define GMAX4002_REG_CTRL 0x2E00
#define GMAX4002_REG_HOLD 0x2E01
#define GMAX4002_REG_EXP0_C_L 0x2E08
#define GMAX4002_REG_EXP0_C_M 0x2E09
#define GMAX4002_REG_EXP0_C_H 0x2E0A
#define GMAX4002_REG_PGA_GAIN 0x2EC9
#define GMAX4002_REG_PWR_UP 0x3301
#define GMAX4002_REG_PWR_DOWN 0x3302

#define GMAX4002_REG_OTP_ADDR 0x3401
#define GMAX4002_REG_OTP_DATA 0x3402

#define GMAX4002_OTP_SENSOR_NAME_BYTE 0x19
#define GMAX4002_OTP_SENSOR_NAME_SHIFT 6

#define GMAX4002_REG_NR_MIN_L 0x2E0F
#define GMAX4002_REG_NR_MIN_H 0x2E10
#define GMAX4002_EXPOSURE_MARGIN 4
#define GMAX4002_TFOT_LINES 22

#define GMAX4002_GAIN_MIN     0x01
#define GMAX4002_GAIN_MAX     0x0f
#define GMAX4002_GAIN_STEP    1
#define GMAX4002_GAIN_DEFAULT 0x01

struct gmax4002_regval {
	u16 addr;
	u8 val;
};

static const s64 gmax4002_link_freqs[] = {
	GMAX4002_LINK_FREQ, // 600 MHz
};

static long gmax4002_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg);

static int gmax4002_s_stream(struct v4l2_subdev *sd, int on);
static int gmax4002_g_frame_interval(struct v4l2_subdev *sd,
									 struct v4l2_subdev_frame_interval *fi);

static int gmax4002_entity_init_cfg(struct v4l2_subdev *sd,
									struct v4l2_subdev_state *state);
static int gmax4002_enum_mbus_code(struct v4l2_subdev *sd,
								   struct v4l2_subdev_state *state,
								   struct v4l2_subdev_mbus_code_enum *code);
static int gmax4002_enum_frame_size(struct v4l2_subdev *sd,
									struct v4l2_subdev_state *state,
									struct v4l2_subdev_frame_size_enum *fse);
static int
gmax4002_enum_frame_interval(struct v4l2_subdev *sd,
							 struct v4l2_subdev_state *state,
							 struct v4l2_subdev_frame_interval_enum *fie);
static int gmax4002_get_fmt(struct v4l2_subdev *sd,
							struct v4l2_subdev_state *state,
							struct v4l2_subdev_format *fmt);
static int gmax4002_set_fmt(struct v4l2_subdev *sd,
							struct v4l2_subdev_state *state,
							struct v4l2_subdev_format *fmt);
static int gmax4002_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
									struct v4l2_mbus_config *cfg);
static int gmax4002_get_selection(struct v4l2_subdev *sd,
								  struct v4l2_subdev_state *state,
								  struct v4l2_subdev_selection *sel);

static int gmax4002_runtime_suspend(struct device *dev);
static int gmax4002_runtime_resume(struct device *dev);
static int gmax4002_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh);
static int gmax4002_s_ctrl(struct v4l2_ctrl *ctrl);

static const struct v4l2_subdev_core_ops gmax4002_core_ops = {
	.ioctl = gmax4002_ioctl,
};

static const struct v4l2_subdev_video_ops gmax4002_video_ops = {
	.s_stream = gmax4002_s_stream,
	.g_frame_interval = gmax4002_g_frame_interval,
};

static const struct v4l2_subdev_pad_ops gmax4002_pad_ops = {
	.init_cfg = gmax4002_entity_init_cfg,
	.enum_mbus_code = gmax4002_enum_mbus_code,
	.enum_frame_size = gmax4002_enum_frame_size,
	.enum_frame_interval = gmax4002_enum_frame_interval,
	.get_fmt = gmax4002_get_fmt,
	.set_fmt = gmax4002_set_fmt,
	.get_mbus_config = gmax4002_get_mbus_config,
	.get_selection = gmax4002_get_selection,
};

static const struct v4l2_subdev_ops gmax4002_subdev_ops = {
	.core = &gmax4002_core_ops,
	.video = &gmax4002_video_ops,
	.pad = &gmax4002_pad_ops,
};

static const struct v4l2_subdev_internal_ops gmax4002_internal_ops = {
	.open = gmax4002_open,
};

static const struct dev_pm_ops gmax4002_pm_ops = { SET_RUNTIME_PM_OPS(
	gmax4002_runtime_suspend, gmax4002_runtime_resume, NULL) };

static const struct media_entity_operations gmax4002_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_ctrl_ops gmax4002_ctrl_ops = {
	.s_ctrl = gmax4002_s_ctrl,
};

static const struct gmax4002_regval gmax4002_10bit_1200m_mipi[] = {
	{ 0x2E00, 0x00 },
	{ 0x2E01, 0x01 },
	{ 0x2E02, 0x00 },
	{ 0x2E03, 0x07 },
	{ 0x2E04, 0x00 },
	{ 0x2E05, 0x08 },
	{ 0x2E06, 0x02 },
	{ 0x2E07, 0x0F },
	{ 0x2E08, 0xCF }, // 0x00, 10ms exposure time
	{ 0x2E09, 0x07 }, // 0x00
	{ 0x2E0A, 0x00 },
	{ 0x2E0B, 0xFF },
	{ 0x2E0C, 0xFF },
	{ 0x2E0D, 0xBE },
	{ 0x2E0E, 0x04 },
	{ 0x2E0F, 0xF4 }, // 0x01, NR_MIN = 6644 lines
	{ 0x2E10, 0x19 }, // 0x00
	{ 0x2E11, 0x12 },
	{ 0x2E12, 0x00 },
	{ 0x2E13, 0x00 },
	{ 0x2E14, 0xBA },
	{ 0x2E15, 0x04 },
	{ 0x2E16, 0x00 },
	{ 0x2E17, 0x00 },
	{ 0x2E18, 0x08 },
	{ 0x2E19, 0x00 },
	{ 0x2E1A, 0xB0 },
	{ 0x2E1B, 0x04 },
	{ 0x2E1C, 0x00 },
	{ 0x2E1D, 0x00 },
	{ 0x2E1E, 0x00 },
	{ 0x2E1F, 0x00 },
	{ 0x2E20, 0x00 },
	{ 0x2E21, 0x00 },
	{ 0x2E22, 0x00 },
	{ 0x2E23, 0x00 },
	{ 0x2E24, 0x00 },
	{ 0x2E25, 0x00 },
	{ 0x2E26, 0x00 },
	{ 0x2E27, 0x00 },
	{ 0x2E28, 0x00 },
	{ 0x2E29, 0x00 },
	{ 0x2E2A, 0x00 },
	{ 0x2E2B, 0x00 },
	{ 0x2E2C, 0x00 },
	{ 0x2E2D, 0x00 },
	{ 0x2E2E, 0x00 },
	{ 0x2E2F, 0x00 },
	{ 0x2E30, 0x00 },
	{ 0x2E31, 0x00 },
	{ 0x2E32, 0x00 },
	{ 0x2E33, 0x00 },
	{ 0x2E34, 0x00 },
	{ 0x2E35, 0x00 },
	{ 0x2E36, 0x00 },
	{ 0x2E37, 0x00 },
	{ 0x2E38, 0x00 },
	{ 0x2E39, 0x00 },
	{ 0x2E3A, 0x00 },
	{ 0x2E3B, 0x00 },
	{ 0x2E3C, 0x00 },
	{ 0x2E3D, 0x00 },
	{ 0x2E3E, 0x00 },
	{ 0x2E3F, 0x00 },
	{ 0x2E40, 0x00 },
	{ 0x2E41, 0x00 },
	{ 0x2E42, 0x00 },
	{ 0x2E43, 0x00 },
	{ 0x2E44, 0x00 },
	{ 0x2E45, 0x00 },
	{ 0x2E46, 0x00 },
	{ 0x2E47, 0x00 },
	{ 0x2E48, 0x00 },
	{ 0x2E49, 0x00 },
	{ 0x2E4A, 0x00 },
	{ 0x2E4B, 0x00 },
	{ 0x2E4C, 0x00 },
	{ 0x2E4D, 0x00 },
	{ 0x2E4E, 0x00 },
	{ 0x2E4F, 0x00 },
	{ 0x2E50, 0x00 },
	{ 0x2E51, 0x00 },
	{ 0x2E52, 0x00 },
	{ 0x2E53, 0x00 },
	{ 0x2E54, 0x00 },
	{ 0x2E55, 0x00 },
	{ 0x2E56, 0x00 },
	{ 0x2E57, 0x00 },
	{ 0x2E58, 0x01 },
	{ 0x2E59, 0x14 },
	{ 0x2E5A, 0x00 },
	{ 0x2E5B, 0x00 },
	{ 0x2E5C, 0x04 },
	{ 0x2E5D, 0x2C }, // 0x22, LINE_TIME = 300
	{ 0x2E5E, 0x01 },
	{ 0x2E5F, 0x02 },
	{ 0x2E60, 0x02 },
	{ 0x2E61, 0x02 },
	{ 0x2E62, 0x28 },
	{ 0x2E63, 0x28 },
	{ 0x2E64, 0x01 },
	{ 0x2E65, 0x00 },
	{ 0x2E66, 0x02 },
	{ 0x2E67, 0x00 },
	{ 0x2E68, 0x24 },
	{ 0x2E69, 0x05 },
	{ 0x2E6A, 0x62 },
	{ 0x2E6B, 0x03 },
	{ 0x2E6C, 0x46 },
	{ 0x2E6D, 0x78 },
	{ 0x2E6E, 0x02 },
	{ 0x2E6F, 0x2A },
	{ 0x2E70, 0xFF },
	{ 0x2E71, 0xFF },
	{ 0x2E72, 0xFF },
	{ 0x2E73, 0xFF },
	{ 0x2E74, 0x32 },
	{ 0x2E75, 0x64 },
	{ 0x2E76, 0x14 },
	{ 0x2E77, 0xFF },
	{ 0x2E78, 0xFF },
	{ 0x2E79, 0xFF },
	{ 0x2E7A, 0x01 },
	{ 0x2E7B, 0x87 },
	{ 0x2E7C, 0xFF },
	{ 0x2E7D, 0xFF },
	{ 0x2E7E, 0x01 },
	{ 0x2E7F, 0x88 },
	{ 0x2E80, 0x05 },
	{ 0x2E81, 0x24 },
	{ 0x2E82, 0xFF },
	{ 0x2E83, 0xFF },
	{ 0x2E84, 0x05 },
	{ 0x2E85, 0x31 },
	{ 0x2E86, 0x5C },
	{ 0x2E87, 0x84 },
	{ 0x2E88, 0x03 },
	{ 0x2E89, 0x0C },
	{ 0x2E8A, 0x13 },
	{ 0x2E8B, 0x34 },
	{ 0x2E8C, 0xFF },
	{ 0x2E8D, 0xFF },
	{ 0x2E8E, 0xFF },
	{ 0x2E8F, 0xFF },
	{ 0x2E90, 0xFF },
	{ 0x2E91, 0xFF },
	{ 0x2E92, 0xFF },
	{ 0x2E93, 0xFF },
	{ 0x2E94, 0x04 },
	{ 0x2E95, 0x0C },
	{ 0x2E96, 0x14 },
	{ 0x2E97, 0x34 },
	{ 0x2E98, 0x01 },
	{ 0x2E99, 0x02 },
	{ 0x2E9A, 0x11 },
	{ 0x2E9B, 0x12 },
	{ 0x2E9C, 0x04 },
	{ 0x2E9D, 0x0C },
	{ 0x2E9E, 0x14 },
	{ 0x2E9F, 0x34 },
	{ 0x2EA0, 0x0B },
	{ 0x2EA1, 0x0C },
	{ 0x2EA2, 0x33 },
	{ 0x2EA3, 0x34 },
	{ 0x2EA4, 0x0C },
	{ 0x2EA5, 0x14 },
	{ 0x2EA6, 0x10 },
	{ 0x2EA7, 0xFF },
	{ 0x2EA8, 0x01 },
	{ 0x2EA9, 0x02 },
	{ 0x2EAA, 0x11 },
	{ 0x2EAB, 0x12 },
	{ 0x2EAC, 0x01 },
	{ 0x2EAD, 0x02 },
	{ 0x2EAE, 0xFF },
	{ 0x2EAF, 0xFF },
	{ 0x2EB0, 0xFF },
	{ 0x2EB1, 0xFF },
	{ 0x2EB2, 0xFF },
	{ 0x2EB3, 0xFF },
	{ 0x2EB4, 0xFF },
	{ 0x2EB5, 0xFF },
	{ 0x2EB6, 0x2E },
	{ 0x2EB7, 0x1C },
	{ 0x2EB8, 0x1E },
	{ 0x2EB9, 0x0C },
	{ 0x2EBA, 0x03 },
	{ 0x2EBB, 0x00 },
	{ 0x2EBC, 0x01 },
	{ 0x2EBD, 0x00 },
	{ 0x2EBE, 0x01 },
	{ 0x2EBF, 0x03 },
	{ 0x2EC0, 0x01 },
	{ 0x2EC1, 0x01 },
	{ 0x2EC2, 0x00 },
	{ 0x2EC3, 0x01 },
	{ 0x2EC4, 0x1E },
	{ 0x2EC5, 0x0C },
	{ 0x2EC6, 0x00 },
	{ 0x2EC7, 0x00 },
	{ 0x2EC8, 0x01 },
	{ 0x2EC9, 0x01 },
	{ 0x2ECA, 0x03 },
	{ 0x2ECB, 0x01 },
	{ 0x3000, 0x01 },
	{ 0x3001, 0x02 },
	{ 0x3002, 0x00 },
	{ 0x3003, 0x00 },
	{ 0x3004, 0x03 },
	{ 0x3005, 0x00 },
	{ 0x3006, 0x08 },
	{ 0x3007, 0x10 },
	{ 0x3008, 0x00 },
	{ 0x3009, 0x04 },
	{ 0x300A, 0x01 },
	{ 0x300B, 0x00 },
	{ 0x300C, 0x01 },
	{ 0x300D, 0x0F },
	{ 0x300E, 0x00 },
	{ 0x300F, 0x01 },
	{ 0x3010, 0x01 },
	{ 0x3011, 0x01 },
	{ 0x3012, 0x00 },
	{ 0x3013, 0x00 },
	{ 0x3014, 0x8E },
	{ 0x3015, 0x09 },
	{ 0x3016, 0x04 },
	{ 0x3017, 0x00 },
	{ 0x3018, 0x08 },
	{ 0x3019, 0x07 },
	{ 0x301A, 0x10 },
	{ 0x301B, 0x07 },
	{ 0x301C, 0x27 },
	{ 0x301D, 0x00 },
	{ 0x301E, 0x0B },
	{ 0x301F, 0x09 },
	{ 0x3020, 0x05 },
	{ 0x3021, 0x06 },
	{ 0x3022, 0x96 },
	{ 0x3023, 0xF8 },
	{ 0x3024, 0x14 },
	{ 0x3025, 0x00 },
	{ 0x3026, 0x00 },
	{ 0x3027, 0x00 },
	{ 0x3028, 0x00 },
	{ 0x3029, 0x00 },
	{ 0x302A, 0x04 },
	{ 0x302B, 0x04 },
	{ 0x302C, 0x04 },
	{ 0x302D, 0x04 },
	{ 0x302E, 0x00 },
	{ 0x302F, 0x00 },
	{ 0x3030, 0x00 },
	{ 0x3031, 0x00 },
	{ 0x3039, 0x00 },
	{ 0x303A, 0x00 },
	{ 0x303B, 0x00 },
	{ 0x303C, 0x00 },
	{ 0x303D, 0x01 }, // disable digital gain
	{ 0x303E, 0x00 },
	{ 0x303F, 0x00 },
	{ 0x3040, 0x10 },
	{ 0x3041, 0x00 },
	{ 0x3042, 0x10 },
	{ 0x3043, 0x00 },
	{ 0x3044, 0x04 },
	{ 0x3045, 0x10 },
	{ 0x3046, 0x00 },
	{ 0x3047, 0x00 },
	{ 0x3048, 0x00 },
	{ 0x3049, 0x00 },
	{ 0x304A, 0x00 },
	{ 0x304B, 0x00 },
	{ 0x304C, 0x00 },
	{ 0x304D, 0x00 },
	{ 0x304E, 0x00 },
	{ 0x304F, 0x02 },
	{ 0x3050, 0x0A },
	{ 0x3051, 0x02 },
	{ 0x3052, 0x00 },
	{ 0x3053, 0x10 },
	{ 0x3054, 0x00 },
	{ 0x3055, 0x00 },
	{ 0x3056, 0x5E },
	{ 0x3057, 0x01 },
	{ 0x3058, 0x00 },
	{ 0x3059, 0x01 },
	{ 0x305A, 0x00 },
	{ 0x305B, 0x3C }, // 0x10, black level offset
	{ 0x305C, 0x00 },
	{ 0x305D, 0x04 },
	{ 0x305E, 0x00 },
	{ 0x305F, 0x00 },
	{ 0x3060, 0x00 },
	{ 0x3200, 0x20 },
	{ 0x3201, 0x00 },
	{ 0x3202, 0x04 },
	{ 0x3203, 0x03 },
	{ 0x3204, 0x20 },
	{ 0x3205, 0x03 },
	{ 0x3206, 0x03 },
	{ 0x3207, 0x03 },
	{ 0x3208, 0x16 },
	{ 0x3209, 0x04 },
	{ 0x320A, 0x00 },
	{ 0x320B, 0x16 },
	{ 0x320C, 0x04 },
	{ 0x320D, 0x1A },
	{ 0x320E, 0x0F },
	{ 0x320F, 0x00 },
	{ 0x3210, 0x11 },
	{ 0x3211, 0x07 },
	{ 0x3212, 0x00 },
	{ 0x3213, 0x0E },
	{ 0x3214, 0x1B },
	{ 0x3215, 0x03 },
	{ 0x3216, 0x3F },
	{ 0x3217, 0x04 },
	{ 0x3218, 0x07 },
	{ 0x3219, 0x00 },
	{ 0x321A, 0x3F },
	{ 0x321B, 0x07 },
	{ 0x321C, 0x00 },
	{ 0x321D, 0x04 },
	{ 0x321E, 0x3F },
	{ 0x321F, 0x04 },
	{ 0x3220, 0x07 },
	{ 0x3221, 0x00 },
	{ 0x3222, 0x26 }, // 0x14
	{ 0x3223, 0x03 },
	{ 0x3224, 0x00 },
	{ 0x3225, 0x00 },
	{ 0x3226, 0x3F },
	{ 0x3227, 0x03 },
	{ 0x3228, 0x00 },
	{ 0x3229, 0x00 },
	{ 0x322A, 0x06 },
	{ 0x322B, 0x03 },
	{ 0x322C, 0x1B },
	{ 0x322D, 0x00 },
	{ 0x322E, 0x07 },
	{ 0x322F, 0x03 },
	{ 0x3230, 0x0E },
	{ 0x3231, 0x41 },
	{ 0x3232, 0x53 },
	{ 0x3233, 0x4E },
	{ 0x3234, 0x47 },
	{ 0x3235, 0x47 },
	{ 0x3236, 0x47 },
	{ 0x3237, 0x47 },
	{ 0x3238, 0x50 },
	{ 0x3239, 0x47 },
	{ 0x323A, 0x53 },
	{ 0x323B, 0x53 },
	{ 0x323C, 0x4A },
	{ 0x323D, 0x4A },
	{ 0x323E, 0x68 },
	{ 0x323F, 0x0A },
	{ 0x3240, 0x00 },
	{ 0x3241, 0x1A },
	{ 0x3242, 0x20 },
	{ 0x3243, 0x04 },
	{ 0x3244, 0x13 },
	{ 0x3245, 0x13 },
	{ 0x3246, 0x30 },
	{ 0x3247, 0x30 },
	{ 0x3248, 0x7D },
	{ 0x3249, 0x1C },
	{ 0x324A, 0x1E },
	{ 0x324B, 0x00 },
	{ 0x324C, 0x00 },
	{ 0x324D, 0x00 },
	{ 0x324E, 0x00 },

	{ 0x3300, 0x00 },
	{ 0x3301, 0x00 },
	{ 0x3302, 0x00 },
	{ 0x3303, 0x00 },
	{ 0x3304, 0x00 },
	{ 0x3305, 0x00 },
	{ 0x3306, 0x00 },
	{ 0x3307, 0x01 },
	{ 0x3308, 0x00 },
	{ 0x3309, 0x01 },
	{ 0x330A, 0x00 },
	{ 0x330B, 0x01 },
	{ 0x330C, 0x00 },
	{ 0x330D, 0x01 },
	{ 0x330E, 0x00 },
	{ 0x330F, 0x01 },
	{ 0x3310, 0x00 },
	{ 0x3311, 0xD0 },
	{ 0x3312, 0x07 },
	{ 0x3313, 0xB8 },
	{ 0x3314, 0x0B },
	{ 0x3315, 0xF4 },
	{ 0x3316, 0x01 },
	{ 0x3317, 0xE8 },
	{ 0x3318, 0x03 },
	{ 0x3319, 0xE8 },
	{ 0x331A, 0x03 },
	{ 0x331B, 0xE8 },
	{ 0x331C, 0x03 },
	{ 0x331D, 0xA0 },
	{ 0x331E, 0x0F },
	{ 0x331F, 0xD0 },
	{ 0x3320, 0x07 },
	{ 0x3321, 0x01 },
	{ 0x3322, 0x00 },
	{ 0x3323, 0x01 },
	{ 0x3324, 0x00 },
	{ 0x3325, 0x01 },
	{ 0x3326, 0x00 },
	{ 0x3327, 0x01 },
	{ 0x3328, 0x00 },
	{ 0x3329, 0x01 },
	{ 0x332A, 0x00 },
	{ 0x332B, 0x01 },
	{ 0x332C, 0x00 },
	{ 0x332D, 0xA0 },
	{ 0x332E, 0x0F },
	{ 0x332F, 0xE8 },
	{ 0x3330, 0x03 },
	{ 0x3331, 0xD0 },
	{ 0x3332, 0x07 },
	{ 0x3333, 0x03 },
	{ 0x3334, 0x00 },
	{ 0x3335, 0xA0 },
	{ 0x3336, 0x0F },
	{ 0x3337, 0xD0 },
	{ 0x3338, 0x07 },
	{ 0x3339, 0xF4 },
	{ 0x333A, 0x01 },
	{ 0x333B, 0x3C },
	{ 0x333C, 0x00 },
	{ 0x333D, 0xB8 },
	{ 0x333E, 0x0B },
	{ 0x333F, 0xE8 },
	{ 0x3340, 0x03 },
	{ 0x3341, 0xE8 },
	{ 0x3342, 0x03 },
	{ 0x3343, 0xE8 },
	{ 0x3344, 0x03 },
	{ 0x3345, 0x55 },
	{ 0x3346, 0x55 },
	{ 0x3347, 0x55 },
	{ 0x3348, 0x55 },
	{ 0x3349, 0x55 },
	{ 0x334A, 0x55 },
	{ 0x334B, 0x55 },
	{ 0x334C, 0x55 },
	{ 0x334D, 0x10 },
	{ 0x334E, 0x32 },
	{ 0x334F, 0x54 },
	{ 0x3350, 0x9A },
	{ 0x3351, 0xCD },
	{ 0x3352, 0x7B },
	{ 0x3353, 0xE8 },
	{ 0x3354, 0x6F },
	{ 0x3355, 0x10 },
	{ 0x3356, 0x42 },
	{ 0x3357, 0x95 },
	{ 0x3358, 0xEA },
	{ 0x3359, 0xCD },
	{ 0x335A, 0x3B },
	{ 0x335B, 0x87 },
	{ 0x335C, 0x6E },
	{ 0x335D, 0x00 },
	{ 0x335E, 0x00 },
	{ 0x335F, 0x00 },
	{ 0x3360, 0x00 },
	{ 0x3361, 0x01 },
	{ 0x3362, 0x00 },
	{ 0x3400, 0x00 },
	{ 0x3401, 0x12 },
	{ 0x3402, 0x00 },
	{ 0x3403, 0x00 },
	{ 0x3404, 0x64 },
	{ 0x3405, 0x02 },
};

struct gmax4002_mode {
	u32 width;
	u32 height;
	u32 max_fps;
	u32 line_time_def;
	u32 nr_min_def;
	const struct gmax4002_regval *reg_list;
	u32 num_regs;
};

static const struct gmax4002_mode supported_modes[] = {
	{
		.width = GMAX4002_WIDTH,
		.height = GMAX4002_HEIGHT,
		.max_fps = GMAX4002_MAX_FPS.denominator,
		.line_time_def = 300,
		.nr_min_def = 6644,
		.reg_list = gmax4002_10bit_1200m_mipi,
		.num_regs = ARRAY_SIZE(gmax4002_10bit_1200m_mipi),
	},
};

struct gmax4002 {
	struct i2c_client *client;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct clk *xvclk;
	struct gpio_desc *reset_gpio;

	u32 module_index;
	const char *module_facing;
	const char *module_name;
	const char *len_name;

	struct v4l2_fwnode_endpoint bus_cfg;
	struct v4l2_mbus_framefmt current_format;
	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *gain;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;

	const struct gmax4002_mode *cur_mode;
	struct mutex mutex; /* Protects device state */
	bool streaming;
};

static inline struct gmax4002 *to_gmax4002(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gmax4002, sd);
}

static int gmax4002_write_reg(struct i2c_client *client, u16 reg, u8 val)
{
	u8 buf[3];
	int ret;

	if (!client)
		return -EINVAL;

	buf[0] = (reg >> 8) & 0xff;
	buf[1] = reg & 0xff;
	buf[2] = val;

	ret = i2c_master_send(client, buf, 3);
	if (ret < 0)
		return ret;
	if (ret != 3)
		return -EIO;

	return 0;
}

static int gmax4002_read_reg(struct i2c_client *client, u16 reg, u8 *val)
{
	struct i2c_msg msgs[2];
	__be16 reg_addr;
	u8 data = 0;
	int ret;

	if (!client || !val)
		return -EINVAL;

	reg_addr = cpu_to_be16(reg);
	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = sizeof(reg_addr);
	msgs[0].buf = (char *)&reg_addr;

	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = sizeof(data);
	msgs[1].buf = &data;

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret < 0)
		return ret;
	if (ret != ARRAY_SIZE(msgs))
		return -EIO;

	*val = data;

	return 0;
}

static int gmax4002_write_array(struct i2c_client *client, const struct gmax4002_regval *reg_list, u32 num_regs)
{
	int ret;
	struct device *dev = &client->dev;

	for (int i = 0; i < num_regs; i++) {
		ret = gmax4002_write_reg(client, reg_list[i].addr, reg_list[i].val);
		if (ret) {
			dev_err(dev, "failed to write register 0x%04x: %d\n",
					reg_list[i].addr, ret);
			return ret;
		}
	}
	return 0;
}

static int gmax4002_read_otp_byte(struct i2c_client *client, u8 otp_addr, u8 *val)
{
	int ret;

	ret = gmax4002_write_reg(client, GMAX4002_REG_OTP_ADDR, otp_addr);
	if (ret)
		return ret;

	ret = gmax4002_read_reg(client, GMAX4002_REG_OTP_DATA, val);
	if (ret)
		return ret;

	return 0;
}

static int gmax4002_check_sensor_id(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	u8 val;
	u64 otp_raw = 0;
	u32 sensor_name;
	u16 id_low;
	u16 id_high;
	int ret;
	int i;

	for (i = 0; i < 5; i++) {
		ret = gmax4002_read_otp_byte(client,
									 GMAX4002_OTP_SENSOR_NAME_BYTE + i,
									 &val);
		if (ret) {
			dev_err(dev, "failed to read OTP byte 0x%02x: %d\n",
					GMAX4002_OTP_SENSOR_NAME_BYTE + i, ret);
			return ret;
		}

		otp_raw |= (u64)val << (i * 8);
	}

	sensor_name = (otp_raw >> GMAX4002_OTP_SENSOR_NAME_SHIFT) & 0xffffffff;

	id_low = sensor_name & 0xffff;
	id_high = (sensor_name >> 16) & 0xffff;

	if (id_low == GMAX4002_CHIP_ID || id_high == GMAX4002_CHIP_ID) {
		dev_info(dev,
				 "detected GMAX4002: sensor_name=0x%08x, id_high=0x%04x, id_low=0x%04x\n",
				 sensor_name, id_high, id_low);
		return 0;
	}

	dev_err(dev,
			"GMAX4002 id mismatch: sensor_name=0x%08x, id_high=0x%04x, id_low=0x%04x, expected=0x%04x\n",
			sensor_name, id_high, id_low, GMAX4002_CHIP_ID);

	return -ENODEV;
}

static int gmax4002_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	struct gmax4002 *gmax4002 = to_gmax4002(sd);
	struct v4l2_mbus_framefmt *try_fmt =
		v4l2_subdev_get_try_format(sd, fh->state, 0);

	mutex_lock(&gmax4002->mutex);

	try_fmt->width = GMAX4002_WIDTH;
	try_fmt->height = GMAX4002_HEIGHT;
	try_fmt->code = GMAX4002_FRAME_CODE;
	try_fmt->field = V4L2_FIELD_NONE;

	mutex_unlock(&gmax4002->mutex);
	return 0;
}

static int gmax4002_setup(struct gmax4002 *gmax4002,
						  struct v4l2_subdev_state *state)
{
	struct i2c_client *client = gmax4002->client;
	int ret = 0;

	dev_info(&client->dev, "gmax4002_setup\n");

	/* Upload configurations */
	ret = gmax4002_write_array(client, gmax4002->cur_mode->reg_list,
							   gmax4002->cur_mode->num_regs);
	if (ret) {
		dev_err(&client->dev, "Failed to upload registers\n");
		return ret;
	}

	usleep_range(5000, 10000);

	// CLK_STABLE_EN = 1
	ret = gmax4002_write_reg(client, GMAX4002_REG_CTRL, 0x01);
	if (ret) {
		dev_err(&client->dev, "Failed to set CLK_STABLE_EN");
		return ret;
	}

	// PWR_UP_EN = 1
	ret = gmax4002_write_reg(client, GMAX4002_REG_PWR_UP, 0x01);
	if (ret) {
		dev_err(&client->dev, "Failed to set PWR_UP_EN");
		return ret;
	}

	usleep_range(80000, 100000);

	// PHY_cal
	ret |= gmax4002_write_reg(client, 0x3024, 0x14);
	usleep_range(1000, 2000);
	ret |= gmax4002_write_reg(client, 0x3023, 0xF8);
	usleep_range(1000, 2000);
	ret |= gmax4002_write_reg(client, 0x3023, 0xF9);
	usleep_range(1000, 2000);
	ret |= gmax4002_write_reg(client, 0x3023, 0xFF);
	usleep_range(1000, 2000);
	ret |= gmax4002_write_reg(client, 0x3024, 0x34);
	usleep_range(1000, 2000);
	ret |= gmax4002_write_reg(client, 0x3023, 0xFB);
	usleep_range(1000, 2000);

	if (ret) {
		dev_err(&client->dev, "Failed to set PHY_cal");
		return ret;
	}

	usleep_range(1000, 2000);

	return 0;
}

static int gmax4002_stream_on(struct gmax4002 *gmax4002)
{
	int ret = 0;
	struct i2c_client *client = gmax4002->client;

	// STREAM_EN = 1
	ret |= gmax4002_write_reg(client, GMAX4002_REG_CTRL, 0x03);

	// REG_HOLD
	ret |= gmax4002_write_reg(client, GMAX4002_REG_HOLD, 0x00);

	if (ret) {
		dev_err(&client->dev, "Failed to set STREAM_EN and REG_HOLD");
		return ret;
	}

	dev_info(&client->dev, "gmax4002_stream_on\n");

	return 0;
}

static int gmax4002_stream_off(struct gmax4002 *gmax4002)
{
	int ret = 0;
	struct i2c_client *client = gmax4002->client;

	// PWR_UP_EN = 0
	ret = gmax4002_write_reg(client, GMAX4002_REG_PWR_UP, 0x00);

	// POW_DOWN_EN = 1
	ret |= gmax4002_write_reg(client, GMAX4002_REG_PWR_DOWN, 0x01);

	if (ret) {
		dev_err(&client->dev,
				"Failed to set POW_DOWN_EN and CLK_STABLE_EN");
		return ret;
	}

	dev_info(&client->dev, "gmax4002_stream_off\n");

	return 0;
}

static long gmax4002_ioctl(struct v4l2_subdev *sd, unsigned int cmd,
						   void *arg)
{
	long ret = 0;
	struct gmax4002 *gmax4002 = to_gmax4002(sd);
	struct rkmodule_inf *inf = arg;

	switch (cmd) {
	case RKMODULE_GET_MODULE_INFO:
		memset(inf, 0, sizeof(struct rkmodule_inf));
		strlcpy(inf->base.sensor, GMAX4002_NAME, sizeof(inf->base.sensor));
		strlcpy(inf->base.module, gmax4002->module_name, sizeof(inf->base.module));
		strlcpy(inf->base.lens, gmax4002->len_name, sizeof(inf->base.lens));
		break;
	case RKMODULE_GET_EXP_INFO:
		ret = -ENOIOCTLCMD;
		break;
	case RKMODULE_GET_EXP_DELAY:
		ret = -ENOIOCTLCMD;
		break;
	case RKMODULE_GET_HDR_CFG:
		ret = -ENOIOCTLCMD;
		break;
	default:
		ret = -ENOIOCTLCMD;
		break;
	}

	return ret;
}

static int gmax4002_s_stream(struct v4l2_subdev *sd, int on)
{
	int ret = 0;
	struct gmax4002 *gmax4002 = to_gmax4002(sd);
	struct i2c_client *client = gmax4002->client;

	dev_info(&client->dev, "s_stream: %d. %dx%d\n", on, GMAX4002_WIDTH,
			 GMAX4002_HEIGHT);

	mutex_lock(&gmax4002->mutex);
	on = !!on;

	if (on == gmax4002->streaming)
		goto unlock_and_return;

	if (on) {
		ret = pm_runtime_get_sync(&client->dev);
		if (ret < 0) {
			pm_runtime_put_noidle(&client->dev);
			goto unlock_and_return;
		}

		ret = gmax4002_setup(gmax4002, NULL);
		if (ret) {
			dev_err(&client->dev, "Failed to setup sensor registers\n");
			goto err_pm;
		}

		/* 重点新增：在 stream_on 之前，同步下发所有缓存在内存中的控制参数 */
		ret = v4l2_ctrl_handler_setup(&gmax4002->ctrl_handler);
		if (ret) {
			dev_err(&client->dev, "Failed to setup v4l2 ctrls\n");
			goto err_pm;
		}

		ret = gmax4002_stream_on(gmax4002);
		if (ret) {
			dev_err(&client->dev, "Failed to start streaming\n");
			goto err_pm;
		}
	} else {
		gmax4002_stream_off(gmax4002);
		pm_runtime_put(&client->dev);
	}

	gmax4002->streaming = on;

	mutex_unlock(&gmax4002->mutex);
	return ret;

err_pm:
	pm_runtime_put(&client->dev);
unlock_and_return:
	mutex_unlock(&gmax4002->mutex);
	return ret;
}

static int gmax4002_g_frame_interval(struct v4l2_subdev *sd,
									 struct v4l2_subdev_frame_interval *fi)
{
    struct gmax4002 *gmax4002 = to_gmax4002(sd);
    u32 vblank = gmax4002->vblank->val;
    u32 frame_length = GMAX4002_HEIGHT + vblank;

	fi->interval.numerator = frame_length  * 5;
	fi->interval.denominator = 1000000;

    return 0;
}

static int gmax4002_entity_init_cfg(struct v4l2_subdev *sd,
									struct v4l2_subdev_state *state)
{
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.format.width = GMAX4002_WIDTH;
	fmt.format.height = GMAX4002_HEIGHT;

	gmax4002_set_fmt(sd, state, &fmt);

	return 0;
}

static int gmax4002_enum_mbus_code(struct v4l2_subdev *sd,
								   struct v4l2_subdev_state *state,
								   struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index != 0)
		return -EINVAL;

	code->code = GMAX4002_FRAME_CODE;

	return 0;
}

static int gmax4002_enum_frame_size(struct v4l2_subdev *sd,
									struct v4l2_subdev_state *state,
									struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != 0 && fse->code != GMAX4002_FRAME_CODE)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = supported_modes[fse->index].width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = supported_modes[fse->index].height;

	return 0;
}

static int
gmax4002_enum_frame_interval(struct v4l2_subdev *sd,
							 struct v4l2_subdev_state *state,
							 struct v4l2_subdev_frame_interval_enum *fi)
{
	if (fi->index != 0)
		return -EINVAL;

	fi->interval = GMAX4002_FRAME_INTERVAL;
	fi->width = GMAX4002_WIDTH;
	fi->height = GMAX4002_HEIGHT;
	fi->code = GMAX4002_FRAME_CODE;

	return 0;
}

static int gmax4002_get_fmt(struct v4l2_subdev *sd,
							struct v4l2_subdev_state *state,
							struct v4l2_subdev_format *fmt)
{
	struct gmax4002 *gmax4002 = to_gmax4002(sd);
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;

	mutex_lock(&gmax4002->mutex);

	framefmt->width = gmax4002->cur_mode->width;
	framefmt->height = gmax4002->cur_mode->height;
	framefmt->code = GMAX4002_FRAME_CODE;
	framefmt->field = V4L2_FIELD_NONE;
	framefmt->colorspace = V4L2_COLORSPACE_RAW;
	framefmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	framefmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	framefmt->xfer_func = V4L2_XFER_FUNC_NONE;

	mutex_unlock(&gmax4002->mutex);
	return 0;
}

static int gmax4002_set_fmt(struct v4l2_subdev *sd,
							struct v4l2_subdev_state *state,
							struct v4l2_subdev_format *fmt)
{
	struct gmax4002 *gmax4002 = to_gmax4002(sd);
	struct v4l2_mbus_framefmt *framefmt = &fmt->format;

	mutex_lock(&gmax4002->mutex);

	if (gmax4002->streaming) {
		mutex_unlock(&gmax4002->mutex);
		return -EBUSY;
	}

	/* Only one mode supported currently */
	gmax4002->cur_mode = &supported_modes[0];

	framefmt->width = gmax4002->cur_mode->width;
	framefmt->height = gmax4002->cur_mode->height;
	framefmt->code = GMAX4002_FRAME_CODE;
	framefmt->field = V4L2_FIELD_NONE;
	framefmt->colorspace = V4L2_COLORSPACE_RAW;

	if (gmax4002->link_freq)
		__v4l2_ctrl_s_ctrl(gmax4002->link_freq, 0);

	if (gmax4002->pixel_rate)
		__v4l2_ctrl_s_ctrl_int64(gmax4002->pixel_rate, gmax4002_link_freqs[0] * 2 *
														   GMAX4002_LANES /
														   GMAX4002_BPP);

	mutex_unlock(&gmax4002->mutex);
	return 0;
}

static int gmax4002_get_mbus_config(struct v4l2_subdev *sd, unsigned int pad_id,
									struct v4l2_mbus_config *config)
{
	config->type = V4L2_MBUS_CSI2_DPHY;
	config->bus.mipi_csi2.num_data_lanes = GMAX4002_LANES;

	return 0;
}

static int gmax4002_get_selection(struct v4l2_subdev *sd,
								  struct v4l2_subdev_state *state,
								  struct v4l2_subdev_selection *sel)
{
	if (!sel || sel->pad != 0)
		return -EINVAL;

	sel->flags = 0;

	if (sel->target == V4L2_SEL_TGT_CROP_BOUNDS) {
		sel->r.left = 0;
		sel->r.width = GMAX4002_WIDTH;
		sel->r.top = GMAX4002_DUMMY_ROWS;
		sel->r.height = GMAX4002_ACTIVE_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

static int __gmax4002_power_on(struct gmax4002 *gmax4002)
{
	int ret = 0;
	struct device *dev = &gmax4002->client->dev;

	dev_info(dev, "--- [GMAX4002] Power On ---\n");
	/* Wait 1ms after power on */
	usleep_range(1000, 2000);

	if (gmax4002->reset_gpio) {
		gpiod_set_value_cansleep(gmax4002->reset_gpio, 0);
		usleep_range(1000, 2000); /* wait for SPI/I2C to be ready */
	}

	return ret;
}

static void __gmax4002_power_off(struct gmax4002 *gmax4002)
{
	struct device *dev = &gmax4002->client->dev;

	dev_info(dev, "--- [GMAX4002] Power Off ---\n");
	if (gmax4002->reset_gpio)
		gpiod_set_value_cansleep(gmax4002->reset_gpio, 1);

	// clk_disable_unprepare(gmax4002->xvclk);
}

static int gmax4002_runtime_resume(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gmax4002 *gmax4002 = to_gmax4002(sd);

	return __gmax4002_power_on(gmax4002);
}

static int gmax4002_runtime_suspend(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct gmax4002 *gmax4002 = to_gmax4002(sd);

	__gmax4002_power_off(gmax4002);
	return 0;
}

static int gmax4002_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gmax4002 *gmax4002 =
		container_of(ctrl->handler, struct gmax4002, ctrl_handler);
	struct i2c_client *client = gmax4002->client;
	unsigned int nr_min;
	int ret = 0;

	/* Only apply changes if sensor is powered up */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	ret = gmax4002_write_reg(client, GMAX4002_REG_HOLD, 0x01);

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		ret |= gmax4002_write_reg(client, GMAX4002_REG_EXP0_C_L,
								(u8)(ctrl->val & 0xff));
		ret |= gmax4002_write_reg(client, GMAX4002_REG_EXP0_C_M,
								(u8)((ctrl->val >> 8) & 0xff));
		ret |= gmax4002_write_reg(client, GMAX4002_REG_EXP0_C_H, 0x00);
		dev_info(&client->dev, "GMAX4002 Debug - Set Exposure: %d", ctrl->val);
		break;

	case V4L2_CID_ANALOGUE_GAIN:
		ret |= gmax4002_write_reg(client,
								GMAX4002_REG_PGA_GAIN,
								(u8)ctrl->val);
		dev_info(&client->dev,
				"GMAX4002 Debug - Set Gain Code: 0x%x",
				ctrl->val);
		break;

	case V4L2_CID_VBLANK:
		nr_min = GMAX4002_HEIGHT + ctrl->val - GMAX4002_TFOT_LINES - 2;
		ret |= gmax4002_write_reg(client, GMAX4002_REG_NR_MIN_L,
								(u8)(nr_min & 0xff));
		ret |= gmax4002_write_reg(client, GMAX4002_REG_NR_MIN_H,
								(u8)((nr_min >> 8) & 0xff));

		__v4l2_ctrl_modify_range(
			gmax4002->exposure,
			gmax4002->exposure->minimum,
			GMAX4002_HEIGHT + ctrl->val - GMAX4002_EXPOSURE_MARGIN - GMAX4002_TFOT_LINES,
			gmax4002->exposure->step,
			min(gmax4002->exposure->default_value,
				GMAX4002_HEIGHT + ctrl->val - GMAX4002_EXPOSURE_MARGIN - GMAX4002_TFOT_LINES));
		dev_info(&client->dev, "GMAX4002 Debug - Set Vblank: %d", nr_min);
		break;

	default:
		dev_warn(&client->dev, "%s unhandled ctrl id:0x%x val:%d\n",
				 __func__, ctrl->id, ctrl->val);
		break;
	}

	ret |= gmax4002_write_reg(client, GMAX4002_REG_HOLD, 0x00);

	pm_runtime_put(&client->dev);
	return ret;
}

static int gmax4002_ctrls_init(struct gmax4002 *gmax4002)
{
	int ret = 0;
	struct i2c_client *client = gmax4002->client;
	struct v4l2_fwnode_device_properties props;

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (ret < 0)
		return ret;

	v4l2_ctrl_handler_init(&gmax4002->ctrl_handler, 9);

	gmax4002->exposure =
		v4l2_ctrl_new_std(&gmax4002->ctrl_handler, &gmax4002_ctrl_ops,
						  V4L2_CID_EXPOSURE, 1, GMAX4002_HEIGHT + GMAX4002_VBLANK_MAX - GMAX4002_TFOT_LINES - GMAX4002_EXPOSURE_MARGIN, 1, 2000);
	gmax4002->gain =
		v4l2_ctrl_new_std(&gmax4002->ctrl_handler, &gmax4002_ctrl_ops,
						  V4L2_CID_ANALOGUE_GAIN, GMAX4002_GAIN_MIN, GMAX4002_GAIN_MAX, GMAX4002_GAIN_STEP, GMAX4002_GAIN_DEFAULT);

	gmax4002->hblank = v4l2_ctrl_new_std(&gmax4002->ctrl_handler, NULL,
										 V4L2_CID_HBLANK, GMAX4002_HBLANK, GMAX4002_HBLANK, 1, GMAX4002_HBLANK);
	if (gmax4002->hblank)
		gmax4002->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	gmax4002->vblank = v4l2_ctrl_new_std(
		&gmax4002->ctrl_handler, &gmax4002_ctrl_ops, V4L2_CID_VBLANK,
		GMAX4002_VBLANK_MIN, GMAX4002_VBLANK_MAX, 1, GMAX4002_VBLANK_MAX);

	gmax4002->pixel_rate = v4l2_ctrl_new_std(
		&gmax4002->ctrl_handler, NULL, V4L2_CID_PIXEL_RATE, 1,
		gmax4002_link_freqs[0] * 2 * GMAX4002_LANES / GMAX4002_BPP, 1,
		gmax4002_link_freqs[0] * 2 * GMAX4002_LANES / GMAX4002_BPP);
	gmax4002->link_freq =
		v4l2_ctrl_new_int_menu(&gmax4002->ctrl_handler, NULL, V4L2_CID_LINK_FREQ,
							   0, 0, gmax4002_link_freqs);

	v4l2_ctrl_new_fwnode_properties(&gmax4002->ctrl_handler, &gmax4002_ctrl_ops,
									&props);

	if (gmax4002->ctrl_handler.error) {
		dev_err(&client->dev, "Failed to add controls (%d)\n",
				gmax4002->ctrl_handler.error);
		v4l2_ctrl_handler_free(&gmax4002->ctrl_handler);
		return gmax4002->ctrl_handler.error;
	}

	gmax4002->sd.ctrl_handler = &gmax4002->ctrl_handler;

	return ret;
}

static int gmax4002_subdev_init(struct gmax4002 *gmax4002)
{
	int ret = 0;
	struct i2c_client *client = gmax4002->client;

	v4l2_i2c_subdev_init(&gmax4002->sd, client, &gmax4002_subdev_ops);
	gmax4002->sd.internal_ops = &gmax4002_internal_ops;

	ret = gmax4002_ctrls_init(gmax4002);
	if (ret < 0)
		return ret;

	gmax4002->sd.entity.ops = &gmax4002_subdev_entity_ops;
	gmax4002->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	gmax4002->pad.flags = MEDIA_PAD_FL_SOURCE;
	gmax4002->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	ret = media_entity_pads_init(&gmax4002->sd.entity, 1, &gmax4002->pad);
	if (ret < 0) {
		v4l2_ctrl_handler_free(&gmax4002->ctrl_handler);
		return ret;
	}

	gmax4002->sd.state_lock = gmax4002->sd.ctrl_handler->lock;
	v4l2_subdev_init_finalize(&gmax4002->sd);

	return ret;
}

static void gmax4002_subdev_cleanup(struct gmax4002 *gmax4002)
{
	media_entity_cleanup(&gmax4002->sd.entity);
	v4l2_ctrl_handler_free(&gmax4002->ctrl_handler);
}

static int gmax4002_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct device_node *node = dev->of_node;
	struct device_node *endpoint;
	struct gmax4002 *gmax4002;
	struct v4l2_subdev *sd;
	int ret = 0;
	char facing;

	dev_info(dev, "driver version : %02d.%02d\n", VERSION_MAJOR, VERSION_MINOR);
	dev_info(dev, "--- [GMAX4002] Probe Start ---\n");

	gmax4002 = devm_kzalloc(dev, sizeof(*gmax4002), GFP_KERNEL);
	if (!gmax4002)
		return -ENOMEM;

	ret = of_property_read_u32(node, RKMODULE_CAMERA_MODULE_INDEX,
							   &gmax4002->module_index);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_FACING,
								   &gmax4002->module_facing);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_MODULE_NAME,
								   &gmax4002->module_name);
	ret |= of_property_read_string(node, RKMODULE_CAMERA_LENS_NAME,
								   &gmax4002->len_name);
	if (ret)
		return -EINVAL;

	gmax4002->client = client;
	gmax4002->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(gmax4002->reset_gpio)) {
		dev_err(dev, "Failed to get reset GPIO descriptor\n");
		return PTR_ERR(gmax4002->reset_gpio);
	}

	if (gmax4002->reset_gpio) {
		gpiod_set_value_cansleep(gmax4002->reset_gpio, 0);
		dev_info(dev, "--- [GMAX4002] Reset PIN successfully initialized to HIGH "
					  "(1.8V) ---\n");
	} else {
		dev_warn(
			dev,
			"--- [GMAX4002] Warning: reset-gpios is NOT defined in DTS! ---\n");
	}

	endpoint = of_graph_get_next_endpoint(node, NULL);
	if (!endpoint)
		return -EINVAL;
	v4l2_fwnode_endpoint_parse(of_fwnode_handle(endpoint), &gmax4002->bus_cfg);
	of_node_put(endpoint);

	mutex_init(&gmax4002->mutex);

	sd = &gmax4002->sd;
	sd->dev = &client->dev;

	ret = gmax4002_subdev_init(gmax4002);
	if (ret < 0)
		goto err_cleanup;

	ret = __gmax4002_power_on(gmax4002);
	if (ret < 0) {
		dev_err(dev, "Failed to power on sensor for ID check\n");
		goto err_cleanup;
	}

	ret = gmax4002_check_sensor_id(client);
	if (ret) {
		dev_err(dev, "Sensor ID verification failed: %d\n", ret);
		goto err_power_off;
	}

	facing = gmax4002->module_facing ? gmax4002->module_facing[0] : 'b';
	snprintf(sd->name, sizeof(sd->name), "m%02d_%c_%s %s", gmax4002->module_index,
			 facing, GMAX4002_NAME, dev_name(dev));

	ret = v4l2_async_register_subdev_sensor(sd);
	if (ret) {
		dev_err(dev, "--- [GMAX4002] Register Failed: %d ---\n", ret);
		goto err_power_off;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_idle(dev);

	dev_info(dev,
			 "--- [GMAX4002] Register Success, Waiting for Handshake... ---\n");
	return 0;

err_power_off:
	__gmax4002_power_off(gmax4002);

err_cleanup:
	gmax4002_subdev_cleanup(gmax4002);
	mutex_destroy(&gmax4002->mutex);

	return ret;
}

static void gmax4002_remote(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gmax4002 *gmax4002 = to_gmax4002(sd);
	struct device *dev = &client->dev;

	dev_info(dev, "--- [GMAX4002] Remote Start ---\n");

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);

	// v4l2_subdev_cleanup(sd);
	v4l2_ctrl_handler_free(&gmax4002->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev)) {
		__gmax4002_power_off(gmax4002);
	}
	pm_runtime_set_suspended(&client->dev);

	mutex_destroy(&gmax4002->mutex);
}

static const struct of_device_id gmax4002_of_macth[] = {
	{ .compatible = "gpixel,gmax4002" },
	{},
};

static struct i2c_driver gmax4002_i2c_driver = {
	.probe_new = gmax4002_probe,
	.remove = gmax4002_remote,
	.driver = {
		.name = GMAX4002_NAME,
		.of_match_table = gmax4002_of_macth,
		.pm = &gmax4002_pm_ops,
	},
};

module_i2c_driver(gmax4002_i2c_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Gpixel GMAX4002 Sensor Driver for RK3588");