// theme.ts - which stylesheets the page is wearing.
//
// A theme is two files: the vendored framework, and the app's layer over it. Everything else -
// structure, spacing, the components - is in app.css and written against tokens, so switching is
// swapping two hrefs rather than reloading a different application.
//
// The same list appears as a lookup table in index.html's <head>, because the choice has to be
// applied before the first paint or the reader sees a flash of the wrong theme on every load. A test
// asserts the two copies agree, since a silent disagreement is a page that flickers.

export interface Theme {
  id: string;
  label: string;
  framework: string;
  skin: string;
  /** One line for the menu: what this theme is FOR, not what it looks like. */
  note: string;
}

export const THEMES: Theme[] = [
  {
    id: 'system6',
    label: 'System 6',
    framework: './vendor/system.css/system.css',
    skin: './themes/system6.css',
    note: 'Mac System 6. One bit, Chicago, window chrome.',
  },
  {
    id: 'plain',
    label: 'Plain',
    framework: './vendor/water.css/water.css',
    skin: './themes/plain.css',
    note: 'White paper, system font. The one for reading the manuals.',
  },
  {
    id: 'dark',
    label: 'Dark',
    framework: './vendor/water.css/dark.css',
    skin: './themes/dark.css',
    note: 'Plain, on a dark ground. For a dim room.',
  },
];

export const DEFAULT_THEME = THEMES[0].id;
const KEY = 'sk-card-theme';

export function currentTheme(): string {
  try {
    const saved = localStorage.getItem(KEY);
    return THEMES.some((t) => t.id === saved) ? saved! : DEFAULT_THEME;
  } catch {
    return DEFAULT_THEME; // private mode - the default is a fine answer
  }
}

/** Swap the two stylesheet links, and remember the choice. No reload: CSS is all that changes. */
export function applyTheme(id: string): void {
  const theme = THEMES.find((t) => t.id === id) ?? THEMES[0];
  const framework = document.getElementById('theme-framework') as HTMLLinkElement | null;
  const skin = document.getElementById('theme-skin') as HTMLLinkElement | null;
  if (framework) framework.href = theme.framework;
  if (skin) skin.href = theme.skin;
  try {
    localStorage.setItem(KEY, theme.id);
  } catch {
    /* the theme still applies for this visit */
  }
}
