// hello - K93OS app in Zig. libk93 is plain C, so Zig talks to it through
// extern declarations (no cImport header parsing needed).
const ui = struct {
    extern fn ui_init(name: [*:0]const u8) c_int;
    extern fn ui_menu(title: [*:0]const u8, items: [*]const [*:0]const u8, n: c_int, sel: c_int) c_int;
    extern fn ui_msgbox(title: [*:0]const u8, msg: [*:0]const u8) void;
    extern fn ui_shutdown() void;
};

pub fn main() void {
    if (ui.ui_init("HelloZig") < 0) return;
    const items = [_][*:0]const u8{ "Say hello", "Quit" };
    while (true) {
        const sel = ui.ui_menu("HelloZig", &items, 2, 0);
        if (sel < 0 or sel == 1) break;
        ui.ui_msgbox("HelloZig", "Hello from Zig on K93OS-x86!");
    }
    ui.ui_shutdown();
}
