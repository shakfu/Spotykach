// context.ts - what every view is handed at mount.
//
// Deliberately tiny. The old context carried the current card between tabs, which sounded like sharing
// and was really coupling: Verify wrote it, nothing read it, and it made the tabs' order of use part of
// their contract. Each view now owns its own model.

import type { Layout } from '../core/layout.ts';

export interface ViewContext {
  layout: Layout;
  /** Bundled example patches for the chuck/csound banks, `{"chuck/0.ck": "..."}`. */
  patches: Record<string, string>;
}

export type MountFn = (root: HTMLElement, ctx: ViewContext) => void;
