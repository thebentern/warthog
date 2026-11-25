# Wiki source

These pages are the GitHub wiki, kept in the main repository so they are
reviewed and versioned with the code that they document.

The wiki is a separate git repository (`<repo>.wiki.git`) and page names come
from filenames — `Quick-Start.md` becomes the *Quick Start* page, `_Sidebar.md`
is the navigation on every page, `Home.md` is the landing page.

## Publishing

Publish the merged pages to the separate wiki repository. Keep this README in
the main repository and copy the other Markdown files, including `_Sidebar.md`.
[Live wiki](https://github.com/thebentern/warthog/wiki)

```bash
wiki_checkout=$(mktemp -d)
git clone https://github.com/thebentern/warthog.wiki.git "$wiki_checkout"
for page in wiki/*.md; do
  [ "$page" = wiki/README.md ] || cp "$page" "$wiki_checkout/"
done
git -C "$wiki_checkout" diff --check
git -C "$wiki_checkout" add -- '*.md'
git -C "$wiki_checkout" commit -m "docs: sync wiki from main repo"
git -C "$wiki_checkout" push
```

## Scope

The wiki is task-oriented: how to flash a board, how to bring up a mode, what to
do when it does not work. Deep technical material — design notes, hardware
findings, protocol detail — belongs in `docs/`, versioned next to the code, and
should be linked from the wiki rather than duplicated into it.
