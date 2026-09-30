/*
 * Smoke test for the PvZ libretro core's game-data resolution.
 *
 * It drives the core through the libretro API without creating a GL context, so
 * it verifies the BIOS / content lookup rules and core bring-up, not rendering:
 *
 *   1. no content + <system>/pvz populated        -> must load (BIOS mode)
 *   2. content = a main.pak                       -> must load (content mode)
 *   3. no content + empty system dir              -> must fail with a message
 *   4. content = a bogus file                     -> must fail with a message
 *
 * Build: tools\smoke\build.bat
 */

#include <windows.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "libretro.h"

static unsigned (*p_retro_api_version)(void);
static void (*p_retro_set_environment)(retro_environment_t);
static void (*p_retro_set_video_refresh)(retro_video_refresh_t);
static void (*p_retro_set_audio_sample)(retro_audio_sample_t);
static void (*p_retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
static void (*p_retro_set_input_poll)(retro_input_poll_t);
static void (*p_retro_set_input_state)(retro_input_state_t);
static void (*p_retro_init)(void);
static void (*p_retro_deinit)(void);
static void (*p_retro_get_system_info)(struct retro_system_info *);
static void (*p_retro_get_system_av_info)(struct retro_system_av_info *);
static bool (*p_retro_load_game)(const struct retro_game_info *);
static void (*p_retro_unload_game)(void);
static size_t (*p_retro_serialize_size)(void);
static bool (*p_retro_serialize)(void *, size_t);
static bool (*p_retro_unserialize)(const void *, size_t);

static const char *g_system_dir = NULL;
static char        g_last_message[1024];
static bool        g_got_message;

/*
 * Frontends walk these two arrays until they hit a NULL terminator; an
 * unterminated array makes them read past the end (RetroArch dumps every port
 * while loading and crashed on exactly that).  Mirror the walk here so the
 * contract is covered by the smoke test.
 */
#define DESC_SCAN_LIMIT 64
#define PORT_SCAN_LIMIT 16

static int  g_desc_count;
static bool g_desc_terminated;
static int  g_port_count;
static bool g_port_terminated;
static int  g_port0_types = -1;

/* The core must hand the frontend a keyboard callback: profile names are typed,
   and a frontend that cannot deliver text would soft-lock the first launch. */
static retro_keyboard_event_t g_kb_event;

/*
 * Core options carry the same termination and default-value contracts as the
 * descriptor arrays: the definition array ends with a zeroed entry, and every
 * default_value must be one of the option's own values or the frontend ignores
 * the option entirely.
 */
#define OPTION_SCAN_LIMIT 64

static int  g_option_count;
static bool g_option_terminated;
static bool g_option_defaults_valid = true;
static char g_first_option_key[128];

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
    static const char *names[] = { "debug", "info", "warn", "error" };
    va_list ap;
    va_start(ap, fmt);
    if (level >= RETRO_LOG_WARN)
    {
        fprintf(stderr, "    core[%s] ", names[level & 3]);
        vfprintf(stderr, fmt, ap);
    }
    va_end(ap);
}

static bool env_cb(unsigned cmd, void *data)
{
    switch (cmd)
    {
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            ((struct retro_log_callback *)data)->log = log_cb;
            return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            *(const char **)data = g_system_dir;
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *(const char **)data = "smoke-saves";
            return true;
        /* Pretend to be a GL frontend so load_game gets past the renderer check. */
        case RETRO_ENVIRONMENT_SET_HW_RENDER:
            return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE:
        {
            const struct retro_message *m = (const struct retro_message *)data;
            g_got_message = true;
            snprintf(g_last_message, sizeof(g_last_message), "%s", m->msg ? m->msg : "");
            return true;
        }
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            return true;

        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        {
            const struct retro_input_descriptor *d =
                (const struct retro_input_descriptor *)data;
            int n = 0;
            while (n < DESC_SCAN_LIMIT && d[n].description != NULL)
                n++;
            g_desc_count      = n;
            g_desc_terminated = (n < DESC_SCAN_LIMIT);
            return true;
        }

        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        {
            const struct retro_controller_info *ci =
                (const struct retro_controller_info *)data;
            int i = 0;
            while (i < PORT_SCAN_LIMIT && ci[i].types != NULL)
                i++;
            g_port_count      = i;
            g_port_terminated = (i < PORT_SCAN_LIMIT);
            g_port0_types     = (i > 0) ? (int)ci[0].num_types : -1;
            return true;
        }

        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *(bool *)data = true;
            return true;
        case RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK:
            g_kb_event = ((const struct retro_keyboard_callback *)data)->callback;
            return true;

        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        {
            const struct retro_core_options_v2 *o =
                (const struct retro_core_options_v2 *)data;
            const struct retro_core_option_v2_definition *d;
            int i;

            if (o == NULL || o->definitions == NULL)
                return true;

            d = o->definitions;
            for (i = 0; i < OPTION_SCAN_LIMIT; i++)
            {
                int j;
                bool found = false;

                if (d[i].key == NULL || d[i].key[0] == 0)
                    break;

                /* libretro: default_value must match one of the values, or the
                   frontend discards the whole option. */
                for (j = 0; j < RETRO_NUM_CORE_OPTION_VALUES_MAX; j++)
                {
                    if (d[i].values[j].value == NULL)
                        break;
                    if (d[i].default_value != NULL &&
                        strcmp(d[i].values[j].value, d[i].default_value) == 0)
                        found = true;
                }
                if (!found)
                    g_option_defaults_valid = false;
            }

            g_option_count      = i;
            g_option_terminated = (i < OPTION_SCAN_LIMIT);
            if (i > 0)
                snprintf(g_first_option_key, sizeof(g_first_option_key), "%s",
                         d[0].key);
            return true;
        }

        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *(bool *)data = false;
            return true;
        default:
            return false;
    }
}

static void noop_video(const void *d, unsigned w, unsigned h, size_t p) { (void)d;(void)w;(void)h;(void)p; }
static void noop_sample(int16_t l, int16_t r) { (void)l;(void)r; }
static size_t noop_batch(const int16_t *d, size_t f) { (void)d; return f; }
static void noop_poll(void) {}
static int16_t noop_state(unsigned a, unsigned b, unsigned c, unsigned d) { (void)a;(void)b;(void)c;(void)d; return 0; }

#define SYM(name) do { \
        p_##name = (void *)GetProcAddress(lib, #name); \
        if (!p_##name) { printf("  MISSING SYMBOL: %s\n", #name); return 2; } \
    } while (0)

static int run_case(HMODULE lib, const char *title, const char *content_path,
                    const char *system_dir, bool expect_ok)
{
    struct retro_game_info game;
    bool ok;

    printf("\n== %s ==\n", title);
    g_system_dir = system_dir;
    g_got_message = false;
    g_last_message[0] = 0;

    SYM(retro_api_version);
    SYM(retro_set_environment);
    SYM(retro_set_video_refresh);
    SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch);
    SYM(retro_set_input_poll);
    SYM(retro_set_input_state);
    SYM(retro_init);
    SYM(retro_deinit);
    SYM(retro_get_system_info);
    SYM(retro_get_system_av_info);
    SYM(retro_load_game);
    SYM(retro_unload_game);
    SYM(retro_serialize_size);
    SYM(retro_serialize);
    SYM(retro_unserialize);

    p_retro_set_environment(env_cb);
    p_retro_set_video_refresh(noop_video);
    p_retro_set_audio_sample(noop_sample);
    p_retro_set_audio_sample_batch(noop_batch);
    p_retro_set_input_poll(noop_poll);
    p_retro_set_input_state(noop_state);

    /* Array-termination contract, checked the way frontends walk them. */
    printf("  input descs: %d entr%s, terminated: %s\n",
           g_desc_count, g_desc_count == 1 ? "y" : "ies",
           g_desc_terminated ? "yes" : "NO");
    printf("  ctrl ports : %d entr%s, terminated: %s, port1 types: %d\n",
           g_port_count, g_port_count == 1 ? "y" : "ies",
           g_port_terminated ? "yes" : "NO", g_port0_types);

    if (!g_desc_terminated || g_desc_count == 0 ||
        !g_port_terminated || g_port_count != 1 || g_port0_types < 1)
    {
        printf("  RESULT     : *** FAIL *** (malformed descriptor arrays)\n");
        p_retro_deinit();
        return 1;
    }

    printf("  keyboard   : %s\n", g_kb_event ? "callback registered" : "NONE");
    if (!g_kb_event)
    {
        printf("  RESULT     : *** FAIL *** (no keyboard callback: text entry is impossible)\n");
        p_retro_deinit();
        return 1;
    }

    printf("  options    : %d entr%s, terminated: %s, defaults valid: %s\n",
           g_option_count, g_option_count == 1 ? "y" : "ies",
           g_option_terminated ? "yes" : "NO",
           g_option_defaults_valid ? "yes" : "NO");
    if (!g_option_terminated || g_option_count == 0 || !g_option_defaults_valid)
    {
        printf("  RESULT     : *** FAIL *** (malformed core options)\n");
        p_retro_deinit();
        return 1;
    }
    printf("  first opt  : %s\n", g_first_option_key);

    /* Frontends may deliver key events at any time, so the callback has to be
       safe before the game exists.  A key press plus its character is what a
       swallowed key looks like after the core's raw-state poll recovers it. */
    g_kb_event(true, RETROK_a, 'a', 0);
    g_kb_event(false, RETROK_a, 0, 0);

    p_retro_init();

    memset(&game, 0, sizeof(game));
    game.path = content_path;

    ok = p_retro_load_game(content_path ? &game : NULL);

    printf("  system dir : %s\n", system_dir ? system_dir : "(null)");
    printf("  content    : %s\n", content_path ? content_path : "(none)");
    printf("  load_game  : %s\n", ok ? "OK" : "FAILED");
    if (g_got_message)
        printf("  message    : %s\n", g_last_message);

    if (ok)
    {
        struct retro_system_av_info av;
        memset(&av, 0, sizeof(av));
        p_retro_get_system_av_info(&av);
        printf("  av info    : %ux%u @ %.1f fps, %.0f Hz\n",
               av.geometry.base_width, av.geometry.base_height,
               av.timing.fps, av.timing.sample_rate);
    }

    /*
     * Save states cover a level in progress, and this run never gets the game
     * past initialisation, so both directions have to refuse.  What is checked
     * here is the contract: a non-zero constant size, and no crash (or bogus
     * success) when there is nothing to snapshot.
     */
    {
        size_t aSize = p_retro_serialize_size();
        void *aBuffer = (aSize > 0) ? malloc(aSize) : NULL;
        bool aSaved = false;
        bool aLoaded = false;

        if (aBuffer != NULL)
        {
            memset(aBuffer, 0, aSize);
            aSaved  = p_retro_serialize(aBuffer, aSize);
            aLoaded = p_retro_unserialize(aBuffer, aSize);
        }
        printf("  save state : %lu bytes, save with no level: %s, load garbage: %s\n",
               (unsigned long)aSize,
               aSaved ? "SAVED (unexpected)" : "refused",
               aLoaded ? "ACCEPTED (unexpected)" : "refused");
        free(aBuffer);

        if (aSize == 0 || aSaved || aLoaded)
        {
            printf("  RESULT     : *** FAIL *** (save-state contract)\n");
            p_retro_unload_game();
            p_retro_deinit();
            return 1;
        }
    }

    if (ok)
        p_retro_unload_game();
    p_retro_deinit();

    if (ok == expect_ok)
    {
        printf("  RESULT     : pass (expected %s)\n", expect_ok ? "load" : "failure");
        return 0;
    }
    printf("  RESULT     : *** FAIL *** (expected %s)\n", expect_ok ? "load" : "failure");
    return 1;
}

int main(int argc, char **argv)
{
    const char *core;
    HMODULE lib;
    int failures = 0;
    char system_pvz[1024];
    char empty_system[1024];
    char bogus[1024];
    struct retro_system_info info;

    if (argc < 5)
    {
        printf("usage: %s <core.dll> <system-parent-dir> <content-main.pak> <workdir>\n", argv[0]);
        return 2;
    }
    core = argv[1];

    snprintf(system_pvz, sizeof(system_pvz), "%s", argv[2]);
    snprintf(empty_system, sizeof(empty_system), "%s\\empty-system", argv[4]);
    CreateDirectoryA(empty_system, NULL);
    snprintf(bogus, sizeof(bogus), "%s\\notagame.pak", argv[4]);
    {
        FILE *f = fopen(bogus, "wb");
        if (f) { fwrite("nope", 1, 4, f); fclose(f); }
    }

    lib = LoadLibraryA(core);
    if (!lib)
    {
        printf("cannot load core %s (error %lu)\n", core, GetLastError());
        return 2;
    }

    SYM(retro_get_system_info);
    memset(&info, 0, sizeof(info));
    p_retro_get_system_info(&info);
    printf("core        : %s %s\n", info.library_name, info.library_version);
    printf("extensions  : %s\n", info.valid_extensions ? info.valid_extensions : "(none)");
    printf("need_fullpath=%d block_extract=%d\n", info.need_fullpath, info.block_extract);

    failures += run_case(lib, "BIOS mode: no content, <system>/pvz populated",
                         NULL, system_pvz, true);
    failures += run_case(lib, "Content mode: main.pak loaded directly",
                         argv[3], empty_system, true);
    failures += run_case(lib, "No content and no BIOS: must report a clear error",
                         NULL, empty_system, false);
    failures += run_case(lib, "Content is not a main.pak: must report a clear error",
                         bogus, empty_system, false);

    FreeLibrary(lib);

    printf("\n%s (%d failure(s))\n", failures ? "SMOKE TEST FAILED" : "SMOKE TEST PASSED", failures);
    return failures ? 1 : 0;
}
