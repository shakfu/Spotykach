// verify_model.ts - run the checker against whatever card the user produced.
//
// The model does not know how a card was obtained: the view hands it a `Card`, which is a plain data
// shape, so the same code path serves the directory picker, a drop, and `<input webkitdirectory>` -
// and a test supplies one built from fixtures.

import { summarize, verifyCard, type Summary } from '../core/verify.ts';
import type { Layout } from '../core/layout.ts';
import type { Card, Finding } from '../core/types.ts';
import { Store } from './store.ts';

export interface VerifyState {
  status: string;
  findings: Finding[];
  summary: Summary | null;
  /** True once a card has been checked - distinguishes "clean card" from "nothing checked yet". */
  checked: boolean;
  /** Whether the checked card can be edited in place, i.e. came with a writable handle. */
  editable: boolean;
  fileCount: number;
  totalBytes: number;
  error: string | null;
  busy: boolean;
}

const INITIAL: VerifyState = {
  status: '', findings: [], summary: null, checked: false, editable: false,
  fileCount: 0, totalBytes: 0, error: null, busy: false,
};

export class VerifyModel {
  readonly store = new Store<VerifyState>({ ...INITIAL });

  constructor(private readonly layout: Layout) {}

  /**
   * Check a card. `getCard` is a thunk so the model owns the whole busy/error lifecycle, including
   * failures that happen while ACQUIRING the card (a dismissed picker, a permission refusal) rather
   * than only those from the walk.
   */
  async run(getCard: () => Promise<Card>): Promise<void> {
    this.store.set({ ...INITIAL, busy: true, status: 'Reading the card...' });
    try {
      const card = await getCard();
      this.store.set({ status: `Checking ${card.files.length} files...` });
      const findings = await verifyCard(this.layout, card);
      const totalBytes = card.files.reduce((n, f) => n + f.size, 0);
      this.store.set({
        busy: false,
        checked: true,
        editable: card.handle != null,
        fileCount: card.files.length,
        totalBytes,
        findings,
        summary: summarize(findings),
        status: '',
      });
    } catch (e) {
      const err = e as Error;
      this.store.set({ busy: false, status: '' });
      // The user dismissed the picker. Not a failure, and showing it as one is worse than silence.
      if (err.name !== 'AbortError') this.store.set({ error: err.message });
    }
  }
}
