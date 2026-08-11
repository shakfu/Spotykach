// run.ts - the entry point. `bun test/run.ts`, which `make test-web` calls.
//
// Bun executes TypeScript directly, so these tests import the same `src/` the bundler consumes rather
// than a compiled copy of it - there is no build step between editing a module and testing it.

import { run } from './harness.ts';

import './wav.test.ts';
import './verify.test.ts';
import './build.test.ts';
import './terminal.test.ts';
import './osc.test.ts';
import './oscdevice.test.ts';
import './client.test.ts';
import './flash.test.ts';
import './misc.test.ts';
import './cardsource.test.ts';
import './model.test.ts';
import './engine.test.ts';
import './ui.test.ts';
import './offline.test.ts';

process.exit(await run());
