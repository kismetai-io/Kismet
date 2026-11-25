# KismetAI notices

Copyright (c) 2026 Artem Tarasov.
KismetAI-owned source and configuration are licensed under Apache License 2.0.
Retain this notice and the accompanying license when redistributing the software.

## Blueprint auto layout

The layered layout solver and its tests under `Source/KismetLayoutCore/` are
ported from [ibrews/blueprint-auto-layout](https://github.com/ibrews/blueprint-auto-layout).
Copyright (c) 2026 Alex Coulombe Presents. The original code is MIT licensed;
the full required notice is in
[ThirdPartyNotices/blueprint-auto-layout-MIT.txt](ThirdPartyNotices/blueprint-auto-layout-MIT.txt).
Kismet adaptations are copyright Artem Tarasov.

## Unreal Engine

KismetAI depends on Unreal Engine, separately licensed by Epic Games. This
source distribution grants no rights to Epic code or assets and includes no
engine installation. Obtain a compatible engine under Epic's applicable terms.
Editor-tool distribution may have additional requirements under the
[Unreal Engine EULA](https://www.unrealengine.com/eula/unreal).

Unreal Engine and Blueprint names identify compatibility. Kismet is independent
and is not sponsored or endorsed by Epic Games.

## Panel UI libraries

The panel UI (`Config/ui.html`) embeds two JavaScript libraries, minified:

- [marked](https://github.com/markedjs/marked) 15.0.7 renders the agent's
  Markdown replies. Copyright (c) 2018+, MarkedJS, and (c) 2011-2018,
  Christopher Jeffrey. MIT licensed; it includes code derived from Markdown,
  Copyright (c) 2004, John Gruber, under a BSD-style license. Both notices are in
  [ThirdPartyNotices/marked-MIT.txt](ThirdPartyNotices/marked-MIT.txt).
- [highlight.js](https://github.com/highlightjs/highlight.js) 11.11.1 highlights
  code blocks. Copyright (c) 2006, Ivan Sagalaev. BSD 3-Clause licensed; the
  notice is in
  [ThirdPartyNotices/highlight.js-BSD-3-Clause.txt](ThirdPartyNotices/highlight.js-BSD-3-Clause.txt).

## Roboto

The panel UI (`Config/ui.html`) embeds a Latin and Cyrillic subset of Roboto
Regular and Medium. Roboto is Copyright Google and licensed under the Apache
License, Version 2.0, the same license as this project.

## Other dependencies

Embedded UI uses system browser facilities. Provider credentials are stored in
encrypted files under the user's Kismet configuration directory, not in an
operating-system credential vault. OpenSSL is linked through Unreal Engine's supplied
third-party module for artifact hashes; its upstream and engine-distribution
notices remain applicable. No engine-supplied OpenSSL source or binaries are
included in the standalone source archive.

Kismet names and logos are not granted as trademarks by the source license.
