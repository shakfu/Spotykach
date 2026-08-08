// context.ts - what every view is handed at mount.
//
// Deliberately tiny. The old context carried the current card between tabs, which sounded like sharing
// and was really coupling: Verify wrote it, nothing read it, and it made the tabs' order of use part of
// their contract. Each view owns its own model.
//
// `engineFocus` is the one piece of genuinely shared state, and it earns its place: the Engines menu
// lives in the page chrome, outside every view, and has to reach into the Reference tab - which may
// not be mounted yet when it is clicked. A store handles both cases without the menu knowing which.

import { Store } from '../app/store.ts';
import type { Catalogue } from '../core/engines.ts';
import type { Layout } from '../core/layout.ts';

export interface EngineFocus {
  engine: string | null;
}

export interface ViewContext {
  layout: Layout;
  engines: Catalogue;
  /** Bundled example patches for the chuck/csound banks, `{"chuck/0.ck": "..."}`. */
  patches: Record<string, string>;
  /** Set by the Engines menu and by `#engine/<name>`; read by the Reference tab. */
  engineFocus: Store<EngineFocus>;
  /**
   * Navigate to another view, or to one engine's page.
   *
   * Injected rather than imported so a view never reaches into the router: the front page's action
   * buttons and the engine catalogue's cards both need to send the reader somewhere, and a direct
   * import would make every one of them untestable without the whole application booted.
   */
  go: (view: string) => void;
  goEngine: (engine: string) => void;
}

export type MountFn = (root: HTMLElement, ctx: ViewContext) => void;
