//! memslammer desktop app: the memory test engine in a GPU-rendered UI
//! (GPUI + gpui-kit). The engine is the same C core the CLI and lite
//! frontends use, compiled in by build.rs.
#![cfg_attr(windows, windows_subsystem = "windows")]

mod app;
mod core;
mod history;

use gpui_kit::component::{Theme, ThemeMode, TitleBar};
use gpui_kit::*;

actions!(memslammer, [Quit]);

fn main() {
    let app = gpui_kit::application().with_assets(gpui_kit::assets::Assets);
    app.run(move |cx| {
        gpui_kit::init(cx);
        cx.bind_keys([
            #[cfg(target_os = "macos")]
            KeyBinding::new("cmd-q", Quit, None),
            #[cfg(not(target_os = "macos"))]
            KeyBinding::new("ctrl-q", Quit, None),
        ]);
        cx.on_action(|_: &Quit, cx: &mut App| cx.quit());
        // Closing the window ends the app; dropping the view stops a running test.
        cx.on_window_closed(|cx, _| {
            if cx.windows().is_empty() {
                cx.quit();
            }
        })
        .detach();

        let options = WindowOptions {
            window_bounds: Some(WindowBounds::centered(size(px(1180.), px(800.)), cx)),
            window_min_size: Some(size(px(900.), px(620.))),
            app_id: Some("memslammer".into()),
            ..TitleBar::window_options()
        };
        gpui_kit::open_window(options, cx, |window, cx| {
            window.activate_window();
            window.set_window_title("memslammer");
            Theme::change(ThemeMode::Dark, Some(window), cx);
            cx.new(|cx| app::MemslammerApp::new(window, cx))
        })
        .expect("failed to open the window");
    });
}
