// SPDX-License-Identifier: MIT
// Tests the delivered HTML/runtime as data in Node; does not open a browser.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {Worker} = require('node:worker_threads');
const {execFileSync} = require('node:child_process');
const {inflateRawSync} = require('node:zlib');
const page = fs.readFileSync(process.argv[2], 'utf8');
const runtime = page.match(/<script id="wasm-runtime" type="text\/plain">([\s\S]*?)<\/script>/)?.[1];
const app = page.match(/<script>([\s\S]*?)<\/script>/)?.[1];
assert(runtime && app, 'embedded runtime and UI scripts exist');
assert(!/<(?:script|link|img|iframe)[^>]+(?:src|href)\s*=\s*["'](?:https?:|\/\/)/i.test(page), 'no remote assets');
assert(page.includes('download="post-profit-exchange-source.zip"'), 'source archive supplied');
const archiveText = page.match(/download="post-profit-exchange-source\.zip" href="data:application\/zip;base64,([A-Za-z0-9+/=]+)"/)?.[1];
assert(archiveText, 'embedded source archive supplied');
const archive = Buffer.from(archiveText, 'base64');
// Read the standard ZIP central directory without trusting local-header sizes.
let end = archive.length-22;
while (end >= Math.max(0,archive.length-65557) && archive.readUInt32LE(end) !== 0x06054b50) end--;
assert(end >= 0 && archive.readUInt32LE(end) === 0x06054b50, 'source ZIP end record');
const entries = new Map();
let cursor = archive.readUInt32LE(end+16);
for (let index=0; index<archive.readUInt16LE(end+10); index++) {
  assert.equal(archive.readUInt32LE(cursor),0x02014b50,'source ZIP central entry');
  const nameLength=archive.readUInt16LE(cursor+28), extraLength=archive.readUInt16LE(cursor+30), commentLength=archive.readUInt16LE(cursor+32);
  const name=archive.subarray(cursor+46,cursor+46+nameLength).toString('utf8');
  assert(!entries.has(name),'source ZIP paths unique');
  entries.set(name,{method:archive.readUInt16LE(cursor+10),size:archive.readUInt32LE(cursor+20),offset:archive.readUInt32LE(cursor+42)});
  cursor+=46+nameLength+extraLength+commentLength;
}
const prefix='projects/post-profit-exchange/';
for (const name of ['configs/exchange.cfg','records/empty-history.json','records/sample-history.json','records/README.md','docs/FILES.md','simulation/main.cpp',
  'simulation/files.hpp','simulation/files.cpp','simulation/files_test.cpp','simulation/models.hpp','simulation/models.cpp',
  'simulation/simulation.cpp','simulation/wasm_test.cjs','simulation/standalone_test.cjs','package-html.py'])
  assert(entries.has(prefix+name),'packaged source file '+name);
assert(![...entries.keys()].some(name=>name.startsWith(prefix+'runs/')),'private run records excluded from distributable');
assert(![...entries.keys()].some(name=>name.startsWith(prefix+'records/') &&
  !['records/empty-history.json','records/sample-history.json','records/README.md'].some(allowed=>name===prefix+allowed)),
  'only empty default and explicit synthetic fixture history are packaged');
for (const name of ['include/ph/exponential_smoothing/ewma.hpp','src/ewma.cpp','CMakeLists.txt','LICENSE','README.md'])
  assert(entries.has('tools/exponential-smoothing/'+name),'independent exponential smoothing source supplied: '+name);
function sourceFile(name) {
  const entry=entries.get(prefix+name); assert(entry,'missing source '+name);
  assert.equal(archive.readUInt32LE(entry.offset),0x04034b50,'source ZIP local entry');
  const start=entry.offset+30+archive.readUInt16LE(entry.offset+26)+archive.readUInt16LE(entry.offset+28);
  const bytes=archive.subarray(start,start+entry.size);
  assert([0,8].includes(entry.method),'supported source ZIP compression');
  return (entry.method===8?inflateRawSync(bytes):bytes).toString('utf8');
}
const bundledConfig=JSON.parse(sourceFile('configs/exchange.cfg'));
const bundledHistory=JSON.parse(sourceFile('records/empty-history.json'));
const syntheticFixtureHistory=JSON.parse(sourceFile('records/sample-history.json'));
assert.equal(bundledConfig.records.history_file,'../records/empty-history.json','default history contains no invented observations');
assert.deepEqual(bundledHistory.observations,[],'empty history means no pre-run observations');
assert.equal(bundledConfig.schema_version,'exchange.run.v1');
assert.equal(bundledHistory.schema_version,'exchange.history.v1');
importRegressions(app,bundledConfig,bundledHistory);
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
    const timer=setTimeout(()=>reject(new Error('worker deadline')),30000);
    worker.once('error',error=>{clearTimeout(timer);reject(error);});
    worker.once('message',value=>{clearTimeout(timer);assert.equal(value.id,id);value.error?reject(new Error(value.error)):resolve(value.result);});
    worker.postMessage({id,command});
  });
}
(async()=>{
  try {
    const defaults=await invoke({op:'defaults'});
    assert.equal(defaults.status,'ok');
    assert.equal(defaults.schema_version,'exchange.sim.v3');
    assert.deepEqual(bundledConfig.simulation,defaults.config,'central cfg is the runtime default source');
    const command={op:'simulate',config:defaults.config,history:bundledHistory};
    const result=await invoke(command);
    assert.equal(result.status,'ok');
    assert.equal(result.objective,'operating_balance_tracking');
    assert.equal(result.schema_version,'exchange.sim.v3');
    assert.deepEqual(result.history,bundledHistory,'worker accepts the packaged record contract');
    const native=JSON.parse(execFileSync(process.argv[3],{input:JSON.stringify(command),encoding:'utf8',maxBuffer:128*1024*1024}));
    compare(result,native,'packaged runtime matches final native source');
    assert.deepEqual(await invoke(command),result,'packaged worker deterministic replay');
    const fixtureCommand={...command,history:syntheticFixtureHistory};
    const fixture=await invoke(fixtureCommand);
    assert.equal(fixture.status,'ok','explicit synthetic history remains a supported test fixture');
    compare(fixture,JSON.parse(execFileSync(process.argv[3],{input:JSON.stringify(fixtureCommand),encoding:'utf8',maxBuffer:128*1024*1024})),
      'explicit synthetic fixture history parity');
    for (const mode of ['optimized','fixed']) {
      assert(result[mode].rows.length<=command.config.periods);
      assert(['completed','insolvent','model_error'].includes(result[mode].summary.terminal_status));
      assert(Array.isArray(result[mode].events));
    }
    const invalid=await invoke({op:'simulate',config:{}});
    assert.equal(invalid.status,'error');
    console.log('Standalone v3: embedded WASM and UI worker match native history/ledger results; replay, failures, central cfg and packaged file contract checks pass. Visual browser testing is separate.');
  } finally {await worker.terminate();}
})().catch(error=>{console.error(error);process.exitCode=1;});

function compare(a,b,field) {
  if (typeof a==='number' && typeof b==='number' && (!Number.isInteger(a)||!Number.isInteger(b))) {
    assert(Math.abs(a-b)<=1e-8*Math.max(1,Math.abs(a)),'floating mismatch '+field);
  } else if (a && b && typeof a==='object' && typeof b==='object') {
    assert.deepEqual(Object.keys(a),Object.keys(b),field);
    for (const key of Object.keys(a)) compare(a[key],b[key],field+'.'+key);
  } else assert.deepEqual(a,b,field);
}

function importRegressions(appSource, config, history) {
  const fieldsStart=appSource.indexOf("  const globalFields = [");
  const helpersStart=appSource.indexOf("  // Strict import helpers begin.");
  const helpersEnd=appSource.indexOf("  // Strict import helpers end.");
  assert(fieldsStart>=0 && helpersStart>fieldsStart && helpersEnd>helpersStart,'strict import helper block present');
  const helpers=vm.runInNewContext(appSource.slice(fieldsStart,helpersStart)+appSource.slice(helpersStart,helpersEnd)+
    '\n({strictJsonParse,validateEditorShape,validateRunManifest,validateHistoryImport});',{TextEncoder});
  const parsed=helpers.strictJsonParse(JSON.stringify(config));
  helpers.validateRunManifest(parsed);
  helpers.validateHistoryImport(helpers.strictJsonParse(JSON.stringify(history)));
  for (const text of [
    '{"a":1,"a":2}', '{"a":1,"\\u0061":2}', '{"nested":{"x":1,"\\u0078":2}}',
    '{"a":1,}', '[true,]', '01', '[1e10000]', '{"x":NaN}', 'true false', '"unescaped\nline"',
    '['.repeat(33)+'0'+']'.repeat(33), ' '.repeat(8*1024*1024+1)
  ]) assert.throws(()=>helpers.strictJsonParse(text),'strict parser rejects malformed/ambiguous input');
  const validDepth=helpers.strictJsonParse('['.repeat(32)+'0'+']'.repeat(32));
  assert(Array.isArray(validDepth),'depth32 is accepted');
  const escaped=helpers.strictJsonParse('{"quote\\\"key":"slash\\\\line\\n","number":-2.5e-1}');
  assert.equal(escaped['quote"key'],'slash\\line\n'); assert.equal(escaped.number,-0.25);
  const proto=helpers.strictJsonParse('{"__proto__":{"polluted":true}}');
  assert(Object.hasOwn(proto,'__proto__')); assert.equal({}.polluted,undefined,'JSON keys cannot change prototypes');
  const rejectConfig=edit=>{
    const document=structuredClone(config); edit(document);
    assert.throws(()=>helpers.validateRunManifest(helpers.strictJsonParse(JSON.stringify(document))));
  };
  rejectConfig(c=>{c.simulation.worker_wages="800";});
  rejectConfig(c=>{c.simulation.seed=true;});
  rejectConfig(c=>{c.simulation.currency=123;});
  rejectConfig(c=>{c.simulation.products[0].candidate_prices[0]="110";});
  rejectConfig(c=>{c.simulation.products[0].unit_cost=null;});
  rejectConfig(c=>{c.simulation.consumers.potential_visitors=false;});
  rejectConfig(c=>{c.simulation.forecast.prior_sigma_units="3";});
  rejectConfig(c=>{c.simulation.scenarios[0].probability="0.1";});
  rejectConfig(c=>{c.run_id="not a valid id";});
  rejectConfig(c=>{c.records.history_file="https://example.invalid/history.json";});
  rejectConfig(c=>{c.records.unexpected=true;});
  rejectConfig(c=>{c.simulation.unknown=0;});
  for (const token of ['800.0','8e2','9007199254740993']) {
    const text=JSON.stringify(config).replace(/"worker_wages":[0-9]+/,'"worker_wages":'+token);
    assert.throws(()=>helpers.validateRunManifest(helpers.strictJsonParse(text)),'money retains exact token kind');
  }
  const decimalProbabilities=JSON.stringify(config).replace(/"prior_sigma_units":[0-9]+/,'"prior_sigma_units":3.0');
  helpers.validateRunManifest(helpers.strictJsonParse(decimalProbabilities));
  const rejectHistory=edit=>{
    const document={schema_version:'exchange.history.v1',observations:[{day:-1,sku:'bread',price:200,sales_units:4,stockout:false}]};
    edit(document); assert.throws(()=>helpers.validateHistoryImport(helpers.strictJsonParse(JSON.stringify(document))));
  };
  rejectHistory(h=>{h.observations[0].price="200";});
  rejectHistory(h=>{h.observations[0].stockout=0;});
  rejectHistory(h=>{h.observations[0].day=false;});
  rejectHistory(h=>{h.observations[0].unknown=1;});
  rejectHistory(h=>{h.extra=[];});
  rejectHistory(h=>{h.observations=Array(12001).fill(h.observations[0]);});
  assert.throws(()=>helpers.validateHistoryImport(helpers.strictJsonParse(
    '{"schema_version":"exchange.history.v1","observations":[{"day":-1,"sku":"bread","price":200.0,"sales_units":4,"stockout":false}]}')));
}
