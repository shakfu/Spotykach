// run.js - the entry point. `node web/test/run.js` or `bun web/test/run.js`; `make test-web` picks one.

import { run } from './harness.js';

import './wav.test.js';
import './verify.test.js';
import './build.test.js';
import './terminal.test.js';
import './misc.test.js';
import './ui.test.js';
import './offline.test.js';

process.exit(await run());
