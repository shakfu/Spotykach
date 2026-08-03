water.css v2.1.1 - https://watercss.kognise.dev/
MIT licensed. Vendored, not fetched from a CDN: the page is a static deploy.

TWO builds, one per theme, and deliberately NOT the `auto` one:
  water.css   out/light.css - the Plain theme
  dark.css    out/dark.css  - the Dark theme

The auto build carries both palettes in one file and follows prefers-color-scheme.
That made a single theme change appearance on its own, so Plain - the plain, white,
for-reading theme - came up on a dark slate ground for anyone whose system was in
dark mode. Splitting them makes the choice the reader's, made in the View menu,
and remembered. Neither file has a prefers-color-scheme rule in it.
