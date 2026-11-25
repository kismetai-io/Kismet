# Panel behaviour tests

The editor panel is one page, `KismetAI/Config/ui.html`, hosted by
`SKismetAIPanel` in UE's web browser widget. These tests load that page,
unchanged, in a real browser engine and drive it the way the editor and the
user do. They need no Unreal Engine and run on every pull request.

```sh
npm ci
npx playwright install chromium webkit
npm test                 # every project
npm run test:chromium    # CEF on Windows and Linux
npm run test:webkit      # WKWebView on macOS
npx playwright test specs/composer.spec.js --headed   # watch one file
```

To test another copy of the page, such as one from a pull request, point
`KISMET_UI_HTML` at it:

```sh
git show origin/some-branch:KismetAI/Config/ui.html > /tmp/ui.html
KISMET_UI_HTML=/tmp/ui.html npm test
```

## What runs

- `contract` (no browser) reads both sides of the page/editor contract from
  source. It fails if the page calls a bridge method `KismetAIBridge.h` does not
  declare, if C++ invokes a `window.on*` callback the page does not define, or
  if the page defines one nothing calls. It also checks that every script block
  parses, nothing loads from the network, and static element ids are unique.
- `chromium` and `webkit` run every other spec in both engines the panel ships in.

## The harness

`support/panel.js` provides a `panel` fixture. It:

- serves `ui.html` with the placeholders `SKismetAIPanel` substitutes
  (`{{KISMETAI_VERSION}}`, `{{KISMETAI_BACKEND_URL}}`);
- installs a fake `window.ue.kismetai` built from the UFUNCTIONs in
  `KismetAIBridge.h`, so the fake has exactly the editor's surface. It records
  every call and answers with per-test responses;
- fails the test on any uncaught page error, any unexpected `console.error`, or
  any network request.

| Call | What it does |
| --- | --- |
| `panel.open({ responses, bridge, html })` | Load the page and wait for `notifyuiready`. `bridge` is `'immediate'`, `'late'` or `'absent'`. |
| `panel.respond(name, value)` | Set a bridge method's resolved value. `panel.sequence(a, b)` gives successive values; `panel.reject(msg)` rejects. |
| `panel.calls(name)` / `panel.callCount(name)` | Arguments of each recorded bridge call. |
| `panel.editor('onX', ...args)` | Invoke a page callback as C++ `ExecuteJavascript` does. |
| `panel.emit(type, data)` / `panel.emitAll([...])` | Send `onAgentEvent` events, as `FKismetAIModule::BroadcastJS` does. |
| `panel.initialState({...})` | `PushInitialState`, with named arguments. |
| `panel.send(text)` | Type into the composer and press Enter. |
| `panel.copyText(message)` | Click a message's copy button and return what reached `setclipboard`. |
| `panel.expandToolCard(card)` | Open the activity log and tool group around a card, then the card. |
| `panel.run('fn', ...args)` | Call a page-level function directly. Use only when no event or click reaches the code. |
| `panel.allowConsoleError(/re/)` | Accept an expected `console.error` in this test. |

Prefer what a user or the editor can do (`send`, clicks, `emit`, `editor`) over
`run`: tests that call internal functions break on refactors that change nothing
a user sees.

## Known bugs

A test that pins down a bug not yet fixed calls `test.fail(true, reason)` at the
top. It passes while the bug exists and fails with "Expected to fail, but
passed" once it is fixed, so the fix must remove the marker and the test then
guards against a regression. Current markers:

- `messages.spec.js`: code blocks are not syntax highlighted (marked v15
  ignores the `highlight` option).
- `replay.spec.js`: a replayed call with no recorded result is shown as a success.
- `tool-cards.spec.js`: a call still in flight when a run stops keeps spinning.
