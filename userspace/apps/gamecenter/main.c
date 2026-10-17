/* K93OS Game Center - local turn-based score store shared by installed games. */
#include <k93ui.h>
#include <k93util.h>
#include <stdio.h>
#include <string.h>

#define GC_DB "/var/k93/gamecenter/scores.tsv"

static void show_scores(char *out, size_t cap)
{
    FILE *f = fopen(GC_DB, "r");
    size_t o = 0;
    o += (size_t)snprintf(out + o, cap - o,
                          "GAME                PLAYER          SCORE\n"
                          "------------------------------------------\n");
    if (!f) {
        snprintf(out + o, cap - o, "No scores yet.\nGames append rows: <game>\\t<player>\\t<score>");
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f) && o + 128 < cap) {
        char g[64] = "", p[64] = "";
        long s = 0;
        if (sscanf(line, "%63[^\t]\t%63[^\t]\t%ld", g, p, &s) == 3)
            o += (size_t)snprintf(out + o, cap - o, "%-20s%-15s%ld\n", g, p, s);
    }
    fclose(f);
}

int main(void)
{
    if (ui_init("GameCenter") < 0) return 1;
    for (;;) {
        char body[4096];
        show_scores(body, sizeof body);
        const char *acts[] = { "Add score", "Back" };
        int a = ui_menu("Game Center - local leaderboard (" GC_DB ")", acts, 2, 0);
        if (a != 0) break;
        char game[64] = "", player[64] = "", score[32] = "";
        if (ui_input("game:", game, sizeof game) != 0) continue;
        if (ui_input("player:", player, sizeof player) != 0) continue;
        if (ui_input("score:", score, sizeof score) != 0) continue;
        k93_mkdir_p("/var/k93/gamecenter", 0755);
        FILE *f = fopen(GC_DB, "a");
        if (f) { fprintf(f, "%s\t%s\t%s\n", game, player, score); fclose(f); }
        ui_msgbox("Game Center", "Score saved.");
    }
    ui_shutdown();
    return 0;
}
