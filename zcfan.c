#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MILLIC_TO_C(n) (n / 1000)
#define TEMP_FILES_GLOB "/sys/class/thermal/thermal_zone*/temp"
#define FAN_CONTROL_FILE "/proc/acpi/ibm/fan"
#define TEMP_INVALID INT_MIN
#define TEMP_MIN INT_MIN + 1

#define max(x, y) ((x) > (y) ? (x) : (y))
#define expect(x)                                                              \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "FATAL: !(%s) at %s:%s:%d\n", #x, __FILE__,        \
                    __func__, __LINE__);                                       \
            abort();                                                           \
        }                                                                      \
    } while (0)

/* Must be highest to lowest temp */
enum FanLevel { FAN_MAX, FAN_MED, FAN_LOW, FAN_OFF, FAN_INVALID };
struct Rule {
    const int tpacpi_level;
    int threshold;
    const char *name;
};
static struct Rule rules[] = {
    [FAN_MAX] = {7, 90, "maximum"},
    [FAN_MED] = {4, 80, "medium"},
    [FAN_LOW] = {2, 70, "low"},
    [FAN_OFF] = {0, TEMP_MIN, "off"},
};

static const unsigned int fan_hysteresis = 10;
static const unsigned int tick_hysteresis = 5;
static char output_buf[512];
static const struct Rule *current_rule = NULL;
static glob_t temp_files;
static volatile sig_atomic_t run = 1;

static int glob_err_handler(const char *epath, int eerrno) {
    fprintf(stderr, "glob: %s: %s\n", epath, strerror(eerrno));
    return 0;
}

static int read_temp_file(const char *filename) {
    FILE *f = fopen(filename, "re");
    int val;

    if (!f) {
        fprintf(stderr, "%s: fopen: %s\n", filename, strerror(errno));
        return -errno;
    }

    expect(fscanf(f, "%d", &val) == 1);
    fclose(f);

    return val;
}

static int get_max_temp(void) {
    int max_temp = TEMP_INVALID;
    int ret = glob(TEMP_FILES_GLOB, 0, glob_err_handler, &temp_files);

    if (ret) {
        expect(ret == GLOB_NOMATCH);
        fprintf(stderr, "Could not find any valid temperature file\n");
        return TEMP_INVALID;
    }

    for (size_t i = 0; i < temp_files.gl_pathc; i++) {
        int temp = read_temp_file(temp_files.gl_pathv[i]);
        max_temp = max(max_temp, temp);
    }
    globfree(&temp_files);

    if (max_temp == TEMP_INVALID) {
        fprintf(stderr, "Couldn't find any valid temperature\n");
        return TEMP_INVALID;
    }

    return MILLIC_TO_C(max_temp);
}

static void write_fan_level(const char *level) {
    FILE *f = fopen(FAN_CONTROL_FILE, "we");
    int ret;

    if (!f) {
        fprintf(stderr, "%s: fopen: %s\n", FAN_CONTROL_FILE, strerror(errno));
        return;
    }

    expect(setvbuf(f, NULL, _IONBF, 0) == 0); /* Make fprintf see errors */
    ret = fprintf(f, "level %s", level);
    if (ret < 0) {
        fprintf(stderr, "%s: write: %s%s\n", FAN_CONTROL_FILE, strerror(errno),
                errno == EINVAL ? " (did you enable fan_control=1?)" : "");
        return;
    }
    fclose(f);
}

static void set_fan_level(void) {
    int max_temp = get_max_temp(), temp_penalty = 0, ret;
    static unsigned int tick_penalty = tick_hysteresis;

    if (tick_penalty > 0) {
        tick_penalty--;
    }

    if (max_temp == TEMP_INVALID) {
        write_fan_level("full-speed");
        return;
    }

    for (size_t i = 0; i < FAN_INVALID; i++) {
        const struct Rule *rule = rules + i;
        char level[sizeof("disengaged")];

        if (rule == current_rule) {
            if (tick_penalty) {
                return; /* Must wait longer until able to move down levels */
            }
            temp_penalty = fan_hysteresis;
        }

        if (rule->threshold < temp_penalty ||
            (rule->threshold - temp_penalty) < max_temp) {
            if (rule != current_rule) {
                current_rule = rule;
                tick_penalty = tick_hysteresis;
                ret = snprintf(level, sizeof(level), "%d", rule->tpacpi_level);
                expect(ret >= 0 && (size_t)ret < sizeof(level));
                printf("[FAN] Temperature now %dC, fan set to %s\n", max_temp,
                       rule->name);
                write_fan_level(level);
            }
            return;
        }
    }

    fprintf(stderr, "No threshold matched?\n");
}

#define CONFIG_PATH "/etc/zcfan.conf"
#define fscanf_threshold(f, pos, name, fl)                                     \
    do {                                                                       \
        int val;                                                               \
        if (fscanf(f, name " %d ", &val) == 1) {                               \
            rules[fl].threshold = val;                                         \
        } else {                                                               \
            expect(fseek(f, pos, SEEK_SET) == 0);                              \
        }                                                                      \
    } while (0)

static void get_config(void) {
    FILE *f;

    f = fopen(CONFIG_PATH, "re");
    if (!f) {
        if (errno != ENOENT) {
            fprintf(stderr, "%s: fopen: %s\n", CONFIG_PATH, strerror(errno));
        }
        return;
    }

    while (!feof(f)) {
        long pos = ftell(f);
        int ch;
        expect(pos >= 0);
        fscanf_threshold(f, pos, "max_temp", FAN_MAX);
        fscanf_threshold(f, pos, "med_temp", FAN_MED);
        fscanf_threshold(f, pos, "low_temp", FAN_LOW);
        if (ftell(f) == pos) {
            while ((ch = fgetc(f)) != EOF && ch != '\n')
                ;
        }
    }
}

static void print_thresholds(void) {
    for (size_t i = 0; i < FAN_OFF; i++) {
        const struct Rule *rule = rules + i;
        printf("[CFG] At %dC fan is set to %s\n", rule->threshold, rule->name);
    }
}

static void stop(int sig) {
    (void)sig;
    run = 0;
}

int main(void) {
    const struct sigaction sa_exit = {
        .sa_handler = stop,
    };

    get_config();
    print_thresholds();
    expect(sigaction(SIGTERM, &sa_exit, NULL) == 0);
    expect(sigaction(SIGINT, &sa_exit, NULL) == 0);
    expect(setvbuf(stdout, output_buf, _IOLBF, sizeof(output_buf)) == 0);

    while (run) {
        set_fan_level();
        if (run) {
            sleep(1);
        }
    }

    printf("[FAN] Quit requested, reenabling thinkpad_acpi fan control\n");
    write_fan_level("auto");
}
