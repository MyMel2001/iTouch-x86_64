/* K93OS TrustMe - profile/certificate installation (peers to old Settings
 * profiles UI). Reads .mobileconfig (XML plist) or PEM cert; installs into
 * the system trust store or the profile store. */
#include <k93ui.h>
#include <k93util.h>
#include <k93plist.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int is_pem(const char *p) { return strstr(p, "-----BEGIN") != NULL; }

int main(void)
{
    if (ui_init("TrustMe") < 0) return 1;
    char path[512] = "/var/mobile/Media/Downloads/profile.mobileconfig";
    if (ui_input("path to .mobileconfig or .pem:", path, sizeof path) != 0) {
        ui_shutdown();
        return 1;
    }
    char *text = k93_read_file(path, NULL);
    if (!text) { ui_msgbox("TrustMe", "Cannot read file (need root?)"); ui_shutdown(); return 1; }

    char msg[768];
    if (is_pem(text)) {
        FILE *f = fopen("/etc/ssl/certs/ca-certificates.crt", "a");
        if (!f) snprintf(msg, sizeof msg, "Cannot write system CA bundle (need root).");
        else {
            fputs(text, f); fputc('\n', f); fclose(f);
            snprintf(msg, sizeof msg,
                     "Certificate installed into system trust store.\n"
                     "Verify: openssl x509 -in %s -noout -subject", path);
        }
    } else {
        char err[128] = "";
        k93_plist *pl = k93_plist_parse(text, strlen(text), err, sizeof err);
        if (!pl) snprintf(msg, sizeof msg, "Not a valid XML plist or PEM certificate:\n%s", err);
        else {
            const char *label = k93_plist_dict_str(pl, "PayloadDisplayName", "unnamed");
            const char *type = k93_plist_dict_str(pl, "PayloadType", "unknown");
            k93_mkdir_p("/var/k93/profiles", 0755);
            char out[600];
            snprintf(out, sizeof out, "/var/k93/profiles/%s.mobileconfig", label);
            k93_write_file(out, text, strlen(text), 0);
            snprintf(msg, sizeof msg,
                     "Profile stored.\n  name: %s\n  type: %s\n  file: %s\n\n"
                     "CA payloads are honored by system TLS via the trust store\n"
                     "(Settings > General > About > Certificates).",
                     label, type, out);
            k93_plist_free(pl);
        }
    }
    ui_msgbox("TrustMe", msg);
    free(text);
    ui_shutdown();
    return 0;
}
