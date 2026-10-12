# Library

## What it is for

The Library reads the ZIM files of the Kiwix library: Wikipedia, Wiktionary, Wikivoyage, the medical encyclopedia, iFixit repair guides, programming language documentation (DevDocs), Gutenberg books and much more, without the Internet. The catalog has thousands of them, in every language, updated every month.

Articles are shown in reading mode: clean text with headings, paragraphs, lists, tables, code and links, in a typeface made for long reading. Navigation menus, notes and edit links are left out.

## How to use it

- **Library**: when it opens you see the collections copied to the `zim` folder, with their cover, language, date, number of articles and size. Tap a collection to open its main page, the **i** for its information (author, source, license, a check of the file's integrity).
- **Continue reading**: at the top of the library, the last article you read, where you left it.
- **Search** (magnifier): titles appear as you type. When no title starts with those words, **Find inside titles** finds them anywhere in a title.
- **Reading**: drag to scroll, tap a link to follow it, press and hold a link to see where it goes. The back arrow returns to the previous article, the forward arrow goes the same way again.
- **Sections** (list): the article's contents; tap a section to go there.
- **Bookmark** (star): adds or removes the article. Bookmarks and history open from the library or the menu.
- **Menu** (three dots): random article, light or dark theme, bookmarks, history, collection information, text size.

## Keyboard and mouse

| Key | What it does |
|---|---|
| Up and down arrows | scroll the article (in lists: choose the row) |
| Page Down, Page Up, Space, Shift+Space | one page forward or back |
| Home, End | start or end of the article |
| Tab, Shift+Tab | move between buttons and links |
| Enter | opens the chosen link or button, the chosen search result |
| Esc, Backspace, Alt+left arrow | back |
| Alt+right arrow | forward |
| `/` or Ctrl+F | search |
| `t` | sections |
| `b` or Ctrl+D | bookmark |
| `r` | random article |
| `+` and `-` | text size |
| `m` | menu |

With a keyboard connected the on-screen keyboard stays hidden. The mouse works like a finger.

## Where to get the files

From the Kiwix library site (library.kiwix.org) download the `.zim` files you want on a PC, then copy them to the `zim` folder of the home folder of the SD card (`/sdcard/home/zim`) with a card reader. Files over 2 GB must be split into parts (`name.zimaa`, `name.zimab`…): the Library reads them as one file.

To start with, the `nopic` (text only) or `mini` (introductions only) versions are a good choice: Italian Wikipedia `top_nopic` is about 900 MB, the Italian Wiktionary about 360 MB, the medical encyclopedia about 90 MB.

The same collections can be read in the Terminal with the `zim` program, and ANIMA uses them to answer.

## Limits

Pictures are not shown yet (they will come with an update): mathematical formulas show their TeX code. Search finds titles, not the words inside articles. Links to the Internet do not open: the Library shows the address. Collections made with zimit (copies of whole websites) look rougher.
