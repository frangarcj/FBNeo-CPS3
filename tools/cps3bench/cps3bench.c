// Headless libretro runner for benchmarking / regression-checking the CPS3 core.
//
//   cps3bench <core.dylib> <rom.zip> [-n frames] [-s sysdir] [-d dumpdir] [-e every] [-q]
//             [-l] [-o key=value]... [-w out.wav]
//
// -l lists the core options, -o sets one, -w writes the audio to a WAV file.
//
// Prints emulated FPS and a per-frame CRC32 of the framebuffer every `every` frames,
// so interpreter and DRC runs can be diffed. With -d, those frames are also dumped as PPM.

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libretro.h"

static const char *sys_dir = ".";
static const char *dump_dir = NULL;
static int every = 60;
static int quiet = 0;
static unsigned pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static int cur_frame = 0;
static uint32_t frame_crc = 0;
static uint64_t audio_frames = 0;
static int list_options = 0;
static FILE *wav = NULL;
static uint32_t wav_frames = 0;

// core options set with -o key=value
#define MAX_OPTS 64
static const char *opt_key[MAX_OPTS], *opt_val[MAX_OPTS];
static int opt_count = 0;

static uint32_t crc_table[256];
static void crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[i] = c;
	}
}
static uint32_t crc32_buf(uint32_t crc, const uint8_t *p, size_t n)
{
	crc = ~crc;
	while (n--) crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
	if (quiet && level < RETRO_LOG_WARN) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static bool env_cb(unsigned cmd, void *data)
{
	switch (cmd & 0xffff) {
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = sys_dir;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
		pixfmt = *(const unsigned *)data;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = log_cb;
		return true;
	case RETRO_ENVIRONMENT_SET_VARIABLES:
		if (list_options)
			for (const struct retro_variable *v = data; v->key; v++)
				fprintf(stderr, "option %s: %s\n", v->key, v->value);
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = (struct retro_variable *)data;
		v->value = NULL;
		for (int i = 0; i < opt_count; i++)
			if (!strcmp(v->key, opt_key[i]))
				v->value = opt_val[i];
		return v->value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = false;
		return true;
	case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
		*(unsigned *)data = 0;
		return true;
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	default:
		return false;
	}
}

static void dump_ppm(const void *data, unsigned w, unsigned h, size_t pitch)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/frame_%06d.ppm", dump_dir, cur_frame);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (unsigned y = 0; y < h; y++) {
		const uint8_t *row = (const uint8_t *)data + y * pitch;
		for (unsigned x = 0; x < w; x++) {
			uint8_t rgb[3];
			if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
				uint32_t p = ((const uint32_t *)row)[x];
				rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p;
			} else if (pixfmt == RETRO_PIXEL_FORMAT_RGB565) {
				uint16_t p = ((const uint16_t *)row)[x];
				rgb[0] = (p >> 11) << 3; rgb[1] = ((p >> 5) & 0x3f) << 2; rgb[2] = (p & 0x1f) << 3;
			} else {
				uint16_t p = ((const uint16_t *)row)[x];
				rgb[0] = ((p >> 10) & 0x1f) << 3; rgb[1] = ((p >> 5) & 0x1f) << 3; rgb[2] = (p & 0x1f) << 3;
			}
			fwrite(rgb, 1, 3, f);
		}
	}
	fclose(f);
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data || every <= 0 || (cur_frame % every) != 0) return;
	size_t bpp = pixfmt == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2;
	uint32_t crc = 0;
	for (unsigned y = 0; y < h; y++)
		crc = crc32_buf(crc, (const uint8_t *)data + y * pitch, w * bpp);
	frame_crc = crc;
	printf("frame %6d %ux%u crc %08x\n", cur_frame, w, h, crc);
	if (dump_dir) dump_ppm(data, w, h, pitch);
}

static void audio_cb(int16_t l, int16_t r)
{
	int16_t s[2] = { l, r };
	audio_frames++;
	if (wav) { fwrite(s, 4, 1, wav); wav_frames++; }
}
static size_t audio_batch_cb(const int16_t *d, size_t frames)
{
	audio_frames += frames;
	if (wav) { fwrite(d, 4, frames, wav); wav_frames += frames; }
	return frames;
}

static void wav_header(FILE *f, uint32_t rate, uint32_t frames)
{
	uint32_t data = frames * 4, riff = 36 + data, fmt = 16, byterate = rate * 4;
	uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
	fwrite(&byterate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id)
{
	(void)port; (void)dev; (void)idx; (void)id;
	return 0;
}

#define LOAD(sym) do { *(void **)&p_##sym = dlsym(h, #sym); if (!p_##sym) { fprintf(stderr, "missing %s\n", #sym); return 1; } } while (0)

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s core rom [-n frames] [-s sysdir] [-d dumpdir] [-e every] [-q] [-l] [-o key=value]\n", argv[0]);
		return 1;
	}
	const char *core = argv[1], *rom = argv[2];
	int frames = 1200;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc) frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) sys_dir = argv[++i];
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) dump_dir = argv[++i];
		else if (!strcmp(argv[i], "-e") && i + 1 < argc) every = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-q")) quiet = 1;
		else if (!strcmp(argv[i], "-l")) list_options = 1;
		else if (!strcmp(argv[i], "-w") && i + 1 < argc) wav = fopen(argv[++i], "wb");
		else if (!strcmp(argv[i], "-o") && i + 1 < argc && opt_count < MAX_OPTS) {
			char *kv = strdup(argv[++i]), *eq = strchr(kv, '=');
			if (eq) { *eq = 0; opt_key[opt_count] = kv; opt_val[opt_count++] = eq + 1; }
		}
	}
	crc_init();

	void *h = dlopen(core, RTLD_NOW | RTLD_LOCAL);
	if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

	void (*p_retro_set_environment)(retro_environment_t);
	void (*p_retro_set_video_refresh)(retro_video_refresh_t);
	void (*p_retro_set_audio_sample)(retro_audio_sample_t);
	void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*p_retro_set_input_poll)(retro_input_poll_t);
	void (*p_retro_set_input_state)(retro_input_state_t);
	void (*p_retro_init)(void);
	void (*p_retro_deinit)(void);
	bool (*p_retro_load_game)(const struct retro_game_info *);
	void (*p_retro_unload_game)(void);
	void (*p_retro_run)(void);
	void (*p_retro_get_system_av_info)(struct retro_system_av_info *);
	LOAD(retro_set_environment); LOAD(retro_set_video_refresh); LOAD(retro_set_audio_sample);
	LOAD(retro_set_audio_sample_batch); LOAD(retro_set_input_poll); LOAD(retro_set_input_state);
	LOAD(retro_init); LOAD(retro_deinit); LOAD(retro_load_game); LOAD(retro_unload_game);
	LOAD(retro_run); LOAD(retro_get_system_av_info);

	p_retro_set_environment(env_cb);
	p_retro_init();
	p_retro_set_video_refresh(video_cb);
	p_retro_set_audio_sample(audio_cb);
	p_retro_set_audio_sample_batch(audio_batch_cb);
	p_retro_set_input_poll(input_poll_cb);
	p_retro_set_input_state(input_state_cb);

	struct retro_game_info gi = { rom, NULL, 0, NULL };
	if (!p_retro_load_game(&gi)) { fprintf(stderr, "load_game failed\n"); return 1; }
	struct retro_system_av_info av;
	p_retro_get_system_av_info(&av);
	fprintf(stderr, "loaded: %ux%u @ %.3f fps\n", av.geometry.base_width, av.geometry.base_height, av.timing.fps);
	if (wav) wav_header(wav, (uint32_t)av.timing.sample_rate, 0);

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (cur_frame = 0; cur_frame < frames; cur_frame++) p_retro_run();
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	printf("%d frames in %.3f s -> %.1f fps (%.0f%% of %.2f), audio frames %llu\n", frames, secs, frames / secs,
	       100.0 * frames / secs / av.timing.fps, av.timing.fps, (unsigned long long)audio_frames);

	if (wav) {
		fseek(wav, 0, SEEK_SET);
		wav_header(wav, (uint32_t)av.timing.sample_rate, wav_frames);
		fclose(wav);
	}

	p_retro_unload_game();
	p_retro_deinit();
	return 0;
}
