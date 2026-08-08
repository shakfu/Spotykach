// theme.ts - which palette the page is wearing.
//
// A theme is now ONE attribute on <html>, not two stylesheet links. The whole app is built from one
// Tailwind stylesheet whose colours are custom properties, so `[data-theme="dark"]` swapping nine
// values is the entire theme - there is no second copy of the app to keep in step, and no second file
// that can fail to load and leave the page bare.
//
// Dark is a CHOICE, not a preference. `prefers-color-scheme` is deliberately NOT consulted: when it
// was, the plain light theme - the one meant for reading the engine manuals - came up on a dark
// ground for anyone whose system was in dark mode, which is the opposite of what it is for. The
// reader picks in the View menu and it is remembered.
//
// The same list appears as a lookup in index.html's <head>, because the choice has to be applied
// before the first paint or a reader who chose Dark gets a white flash on every load. A test asserts
// the two copies agree, since a silent disagreement is a page that flickers.

export interface Theme {
  id: string;
  label: string;
  /** One line for the menu: what this theme is FOR, not what it looks like. */
  note: string;
}

export const THEMES: Theme[] = [
  {
    id: 'light',
    label: 'Light',
    note: 'White paper, system font. The one for reading the manuals.',
  },
  {
    id: 'dark',
    label: 'Dark',
    note: 'The same page on a dark ground. For a dim room.',
  },
];

export const DEFAULT_THEME = THEMES[0].id;
export const STORAGE_KEY = 'sk-card-theme';
/** The attribute the stylesheet keys off. Exported so the head-script test can name it once. */
export const THEME_ATTR = 'data-theme';

export function currentTheme(): string {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    return THEMES.some((t) => t.id === saved) ? saved! : DEFAULT_THEME;
  } catch {
    return DEFAULT_THEME; // private mode - the default is a fine answer
  }
}

/** Set the attribute, and remember the choice. No reload and no fetch: only custom properties change. */
export function applyTheme(id: string): void {
  const theme = THEMES.find((t) => t.id === id) ?? THEMES[0];
  document.documentElement.setAttribute(THEME_ATTR, theme.id);
  try {
    localStorage.setItem(STORAGE_KEY, theme.id);
  } catch {
    /* the theme still applies for this visit */
  }
}
