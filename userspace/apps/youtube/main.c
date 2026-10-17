/* K93OS YouTube - locally hosted video directory (no streaming DRM client). */
#include <k93ui.h>
#include <k93util.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char msg[1024] = "";
    if (ui_init("YouTube") < 0) return 1;
    const char *video_dir = "/var/k93/videos";
    DIR *d = opendir(video_dir);
    if (d) {
        struct dirent *e;
        int n = 0;
        snprintf(msg, sizeof msg, "Locally stored videos in %s:\n\n", video_dir);
        while ((e = readdir(d)) && n < 20) {
            size_t l = strlen(e->d_name);
            if (l < 5 || e->d_name[0] == '.') continue;
            if (!strcasecmp(e->d_name + l - 4, ".mp4") ||
                !strcasecmp(e->d_name + l - 5, ".h264") ||
                !strcasecmp(e->d_name + l - 4, ".mkv")) {
                strncat(msg, "  ", sizeof msg - strlen(msg) - 1);
                strncat(msg, e->d_name, sizeof msg - strlen(msg) - 1);
                strncat(msg, "\n", sizeof msg - strlen(msg) - 1);
                n++;
            }
        }
        closedir(d);
        if (strlen(msg) == 0)
            snprintf(msg, sizeof msg, "No video files in %s.", video_dir);
        else
            strncat(msg, "\nOpen a file with Files > Open With > YouTube.",
                    sizeof msg - strlen(msg) - 1);
    } else {
        snprintf(msg, sizeof msg,
                 "No videos found in %s.\n\n"
                 "Place .mp4/.h264 files there (e.g. extracted from a\n"
                 "third-party content pack) to watch them here.", video_dir);
    }
    ui_msgbox("YouTube", msg);
    ui_shutdown();
    return 0;
}
