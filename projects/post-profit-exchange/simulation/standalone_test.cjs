// SPDX-License-Identifier: MIT
// Tests the delivered HTML/runtime as data in Node; does not open a browser.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {Worker} = require('node:worker_threads');
const {execFileSync} = require('node:child_process');
const page = fs.readFileSync(process.argv[2], 'utf8');
const runtime = page.match(/<script id="wasm-runtime" type="text\/plain">([\s\S]*?)<\/script>/)?.[1];
const app = page.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert(runtime && app, 'embedded runtime and UI scripts exist');
assert(!/<(?:script|link|img|iframe)[^>]+(?:src|href)\s*=\s*["'](?:https?:|\/\/)/i.test(page), 'no remote assets');
assert(page.includes('download="post-profit-exchange-source.zip"'), 'source archive supplied');
new vm.Script(runtime);
new vm.Script(app);
const ids = [...page.matchAll(/\bid="([^"]+)"/g)].map(m=>m[1]);
assert.equal(new Set(ids).size, ids.length, 'unique static element IDs');
for (const match of app.matchAll(/\$\("([^"]+)"\)/g)) assert(ids.includes(match[1]), `missing UI element ${match[1]}`);
const wasm = runtime.match(/findWasmBinary\(\)\{return base64Decode\("([A-Za-z0-9+/=]+)"\)/)?.[1];
assert(wasm && WebAssembly.validate(Buffer.from(wasm, 'base64')), 'valid embedded WebAssembly');
const handler = app.match(/const handler = `([\s\S]*?)`;/)?.[1];
assert(handler, 'worker message handler supplied');
const worker = new Worker(`const {parentPort}=require('node:worker_threads');global.self={postMessage:v=>parentPort.postMessage(v)};${runtime}\n${handler}\nparentPort.on('message', data=>self.onmessage({data}));`, {eval:true});
let sequence = 0;
function invoke(command) {
  return new Promise((resolve,reject)=> {
    const id=++sequence;
    const timer=setTimeout(()=>reject(new Error('worker deadline')),10000);
    worker.once('error',error=>{clearTimeout(timer);reject(error);});
    worker.once('message',value=>{clearTimeout(timer);assert.equal(value.id,id);value.error?reject(new Error(value.error)):resolve(value.result);});
    worker.postMessage({id,command});
  });
}
(async()=>{
  try {
    const defaults=await invoke({op:'defaults'});
    assert.equal(defaults.status,'ok');
    const command={op:'simulate',config:defaults.config};
    const result=await invoke(command);
    assert.equal(result.status,'ok');
    const native=JSON.parse(execFileSync(process.argv[3],{input:JSON.stringify(command),encoding:'utf8'}));
    assert.deepEqual(result,native,'packaged runtime matches final native source');
    const invalid=await invoke({op:'simulate',config:{}});
    assert.equal(invalid.status,'error');
    console.log('Standalone bundle: embedded WASM valid; actual UI worker handler returns native-equivalent ledgers and structured failures; assets/IDs/source archive checks pass. Visual browser testing is separate.');
  } finally {await worker.terminate();}
})().catch(error=>{console.error(error);process.exitCode=1;});
