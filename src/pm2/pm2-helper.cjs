// NotepadAI PM2 helper. Started by Terminal > PM2 Processes while the dialog
// is open; talks to the user's pm2 daemon through pm2's own API.
//
//   node pm2-helper.cjs [<pm2 package dir>]
//
// stdout: one JSON object per line ({"t":"ready"|"list"|"result"|"fatal"}).
// stdin:  one JSON command per line ({"id":N,"op":...}). Exits when stdin ends.
'use strict';

const fs = require('fs');
const path = require('path');
const os = require('os');
const util = require('util');

// pm2 prints to the console; keep stdout for the protocol.
const writeOut = process.stdout.write.bind(process.stdout);
for (const k of ['log', 'info', 'warn', 'error', 'debug']) {
  console[k] = (...args) => process.stderr.write(util.format(...args) + '\n');
}
function send(obj) { writeOut(JSON.stringify(obj) + '\n'); }
function fatal(message, code) { send({ t: 'fatal', code: code || '', error: message }); process.exit(2); }

function isPm2Package(dir) {
  try {
    return JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8')).name === 'pm2';
  } catch (e) {
    return false;
  }
}

// The pm2 a shell would run: the first `pm2` on PATH (npx puts its package's
// .bin first), followed to its package directory.
function findPm2() {
  const fromArg = process.argv[2];
  if (fromArg && isPm2Package(fromArg)) return fromArg;
  const names = process.platform === 'win32' ? ['pm2.cmd', 'pm2'] : ['pm2'];
  for (const dir of (process.env.PATH || '').split(path.delimiter)) {
    if (!dir) continue;
    for (const name of names) {
      const bin = path.join(dir, name);
      if (!fs.existsSync(bin)) continue;
      const candidates = [];
      try {
        // POSIX installs link bin/pm2 into the package.
        let d = path.dirname(fs.realpathSync(bin));
        for (let i = 0; i < 4; ++i, d = path.dirname(d)) candidates.push(d);
      } catch (e) {}
      candidates.push(path.join(dir, 'node_modules', 'pm2'));     // Windows global npm
      candidates.push(path.join(dir, '..', 'pm2'));                // node_modules/.bin (npx)
      candidates.push(path.join(dir, '..', 'lib', 'node_modules', 'pm2'));
      for (const c of candidates) {
        if (isPm2Package(c)) return path.resolve(c);
      }
    }
  }
  return null;
}

const pm2Dir = findPm2();
if (!pm2Dir) fatal('pm2 was not found', 'no-pm2');

let pm2;
let pm2Version = '';
try {
  pm2 = require(pm2Dir);
  pm2Version = JSON.parse(fs.readFileSync(path.join(pm2Dir, 'package.json'), 'utf8')).version || '';
} catch (e) {
  fatal('Could not load pm2: ' + e.message);
}

let dumpPath;
try {
  dumpPath = require(path.join(pm2Dir, 'constants.js')).DUMP_FILE_PATH;
} catch (e) {}
if (!dumpPath) dumpPath = path.join(process.env.PM2_HOME || path.join(os.homedir(), '.pm2'), 'dump.pm2');

function errorText(err) {
  if (!err) return '';
  if (Array.isArray(err)) return err.map(errorText).join('; ');
  return err.message || String(err);
}

// Names in pm2's saved list, re-read only when the file changes.
let dumpCache = { mtimeMs: -1, names: null };
function dumpNames() {
  let st;
  try {
    st = fs.statSync(dumpPath);
  } catch (e) {
    dumpCache = { mtimeMs: -1, names: null };
    return null;
  }
  if (st.mtimeMs !== dumpCache.mtimeMs) {
    let names = [];
    try {
      const apps = JSON.parse(fs.readFileSync(dumpPath, 'utf8'));
      if (Array.isArray(apps)) names = apps.filter((a) => a && !a.pmx_module).map((a) => String(a.name));
    } catch (e) {}
    dumpCache = { mtimeMs: st.mtimeMs, names };
  }
  return dumpCache.names;
}

let listing = false;
let listAgain = false;
function sendList() {
  if (listing) { listAgain = true; return; }
  listing = true;
  pm2.list((err, list) => {
    listing = false;
    if (!err) {
      const procs = [];
      for (const p of list || []) {
        const env = p.pm2_env || {};
        if (env.pmx_module) continue;
        procs.push({
          id: p.pm_id,
          name: p.name,
          status: env.status,
          pid: p.pid || 0,
          startedAt: env.pm_uptime || 0,
          restarts: env.restart_time || 0,
          unstable: env.unstable_restarts || 0,
          cpu: (p.monit && p.monit.cpu) || 0,
          mem: (p.monit && p.monit.memory) || 0,
          cwd: env.pm_cwd || '',
          script: env.pm_exec_path || '',
        });
      }
      send({ t: 'list', procs, dump: dumpNames() });
    }
    if (listAgain) { listAgain = false; sendList(); }
  });
}

let eventTimer = null;
function listSoon() {
  if (eventTimer) return;
  eventTimer = setTimeout(() => { eventTimer = null; sendList(); }, 50);
}

function startOptions(app) {
  const o = {
    name: app.name,
    script: app.script,
    cwd: app.cwd || undefined,
    interpreter: 'node',
    exec_mode: 'fork',
    autorestart: app.autorestart !== false,
    watch: !!app.watch,
  };
  if (o.watch) o.ignore_watch = ['node_modules', '.git'];
  if (app.maxMemoryRestart) o.max_memory_restart = app.maxMemoryRestart;
  if (app.restartDelayMs > 0) o.restart_delay = app.restartDelayMs;
  return o;
}

const call = (fn, ...args) => new Promise((resolve, reject) =>
  fn.call(pm2, ...args, (err, res) => (err ? reject(err) : resolve(res))));

async function eachId(fn, ids) {
  for (const id of ids || []) await call(fn, id);
}

async function run(cmd) {
  switch (cmd.op) {
    case 'list': return;
    case 'start': return call(pm2.start, startOptions(cmd.app));
    case 'stop': return eachId(pm2.stop, cmd.ids);
    case 'restart': return eachId(pm2.restart, cmd.ids);
    case 'delete': return eachId(pm2.delete, cmd.ids);
    case 'recreate':
      await call(pm2.delete, cmd.pmId);
      return call(pm2.start, startOptions(cmd.app));
    // Forced: deleting the last process must be saved too.
    case 'save': return call(pm2.dump, true);
    case 'resurrect': return call(pm2.resurrect);
    default: throw new Error('unknown command ' + cmd.op);
  }
}

// One command at a time, in arrival order.
let queue = Promise.resolve();
function onCommand(line) {
  let cmd;
  try {
    cmd = JSON.parse(line);
  } catch (e) {
    return;
  }
  queue = queue.then(async () => {
    try {
      await run(cmd);
      send({ t: 'result', id: cmd.id, ok: true });
    } catch (err) {
      send({ t: 'result', id: cmd.id, ok: false, error: errorText(err) });
    }
    sendList();
  });
}

function shutdown() {
  try { pm2.disconnect(); } catch (e) {}
  process.exit(0);
}

pm2.connect((err) => {
  if (err) fatal('Could not connect to pm2: ' + errorText(err));
  send({ t: 'ready', pm2Dir, pm2Version, node: process.execPath, dumpPath });
  sendList();

  pm2.launchBus((busErr, bus) => {
    if (!busErr && bus) bus.on('process:event', listSoon);
  });
  setInterval(sendList, 1500);

  let buffered = '';
  process.stdin.setEncoding('utf8');
  process.stdin.on('data', (chunk) => {
    buffered += chunk;
    let nl;
    while ((nl = buffered.indexOf('\n')) >= 0) {
      const line = buffered.slice(0, nl).trim();
      buffered = buffered.slice(nl + 1);
      if (line) onCommand(line);
    }
  });
  process.stdin.on('end', shutdown);
  process.stdin.on('error', shutdown);
});
