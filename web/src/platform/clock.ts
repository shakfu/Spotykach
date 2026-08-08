// clock.ts - setInterval behind the Clock port.
//
// Five lines, and worth the file: with the timer injected, "does the CPU poll stop when the device
// stops answering" is a synchronous unit test instead of a thing you watch for in a browser.

import type { Clock } from '../core/ports.ts';

export const browserClock: Clock = {
  every(ms, fn) {
    const id = setInterval(fn, ms);
    return () => clearInterval(id);
  },
};
