// store.ts - the smallest thing that can be called state management.
//
// Forty lines rather than a framework, because the job is narrow: a view-model holds a state object,
// a view subscribes and re-renders. What it buys is the thing the old code did not have - the state
// lives somewhere a test can read it, so "does Convert disable its buttons until a file is added" is
// answerable without a DOM.
//
// Replacement is deliberate: `set` produces a new object rather than mutating, so a view can compare
// against the state it last rendered and skip work, and a test can hold onto a snapshot.

export type Unsubscribe = () => void;

export class Store<T extends object> {
  private state: T;
  private readonly listeners = new Set<(state: Readonly<T>) => void>();

  constructor(initial: T) {
    this.state = initial;
  }

  get(): Readonly<T> {
    return this.state;
  }

  /** Merge a patch and notify. Listeners see the new state; a listener that throws does not stop the rest. */
  set(patch: Partial<T>): void {
    this.state = { ...this.state, ...patch };
    for (const fn of [...this.listeners]) fn(this.state);
  }

  /** Subscribe, and receive the current state immediately - a view should not have to render twice. */
  subscribe(fn: (state: Readonly<T>) => void): Unsubscribe {
    this.listeners.add(fn);
    fn(this.state);
    return () => this.listeners.delete(fn);
  }
}
