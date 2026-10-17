# touchdova — HTML apps for K93OS-x86

Clean-room counterpart of Apache Cordova: write a web page, wrap it in a
native container, ship it as a normal K93OS app (`.ipa`/`.deb`).

```
touchdova/bin/touchdova new MyApp    # scaffold: www/index.html + container
cd MyApp
../app-sdk/bin/k93sdk build .        # native static x86_64 binary
../app-sdk/bin/k93sdk bundle .       # dist/MyApp.ipa + dist/MyApp.deb
```

## How it works

The generated app is an ordinary K93OS app. Its `main.c` is a **touchdova
container**: it reads `www/index.html` bundled next to the executable,
renders it with the libk93 text renderer (headings, paragraphs, lists,
bold/italic, links), and turns `<a href>` entries into a "Links" menu.

## Native bridge (`k93://`)

| href                    | action                          |
|-------------------------|---------------------------------|
| `k93://alert/<msg>`     | native message box (`%20`=space) |
| `k93://quit`            | close the app                   |
| `https://…` / `http://…`| opens in Safari                 |

Unknown `k93://` actions show a hint instead of failing silently.

## Ceiling

- **No JavaScript engine.** The container renders static HTML and handles
  links; interactivity comes from the bridge scheme. Adding a JS engine
  (e.g. a small interpreter behind `k93://eval`) is the documented upgrade
  path — it is intentionally not shipped: the OS keeps its attack surface
  tiny and every shipped component auditable.
- HTML subset only: text markup + links. No CSS, no images, no forms.
  The renderer is ~120 lines of C in the generated `main.c` — read it,
  extend it, it is your code.
- Windows needs WSL (same as the rest of the SDK).

## Files

```
touchdova/bin/touchdova   # scaffolder (POSIX sh)
```
