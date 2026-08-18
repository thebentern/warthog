# Wiki source

These pages are the GitHub wiki, kept in the main repository so they are
reviewed and versioned with the code that they document.

The wiki is a separate git repository (`<repo>.wiki.git`) and page names come
from filenames — `Quick-Start.md` becomes the *Quick Start* page, `_Sidebar.md`
is the navigation on every page, `Home.md` is the landing page.

## Publishing

GitHub wikis are unavailable on **private repositories on the Free plan**, so
until this repo is public (or the plan changes) the wiki cannot be enabled and
these pages live here only. `has_wiki` silently stays `false` if you try.

Once it is available, enable the wiki in repository settings, create any page
once through the web UI so the wiki repository exists, then:

```bash
git clone https://github.com/thebentern/warthog.wiki.git /tmp/warthog.wiki
cp wiki/*.md /tmp/warthog.wiki/
cd /tmp/warthog.wiki && git add -A && git commit -m "docs: sync wiki from main repo" && git push
```

## Scope

The wiki is task-oriented: how to flash a board, how to bring up a mode, what to
do when it does not work. Deep technical material — design notes, hardware
findings, protocol detail — belongs in `docs/`, versioned next to the code, and
should be linked from the wiki rather than duplicated into it.
