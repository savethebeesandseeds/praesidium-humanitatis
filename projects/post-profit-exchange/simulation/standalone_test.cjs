// SPDX-License-Identifier: MIT
// Tests the delivered HTML/runtime as data in Node; does not open a browser.
// Run with Node: standalone_test.cjs <standalone HTML> <native simulator>
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
    assert.equal(result.objective_version,2);
    assert.equal(result.schema_version,'exchange.sim.v3');
    assert.deepEqual(result.history,bundledHistory,'worker accepts the packaged record contract');
    priceExplanationRegressions(app,result);
    compactInterfaceRegressions(app,page,result);
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

function compactInterfaceRegressions(appSource, html, result) {
  const block=(start,end)=>{
    const first=appSource.indexOf(start), last=appSource.indexOf(end,first);
    assert(first>=0 && last>first,'packaged compact UI helper exists: '+start.trim());
    return appSource.slice(first,last);
  };
  for (const [id,expected] of [
    ['chart-view',['overview','activity','accounts','all']],
    ['selected-policy',['optimized','fixed','both']]
  ]) {
    const select=html.match(new RegExp('<select\\b[^>]*\\bid="'+id+'"[^>]*>([\\s\\S]*?)<\\/select>'))?.[1];
    assert(select,'view selector exists: '+id);
    assert.deepEqual([...select.matchAll(/<option\b[^>]*\bvalue="([^"]+)"/g)].map(match=>match[1]),expected);
  }
  assert(/<input\b(?=[^>]*\bid="ledger-all")(?=[^>]*\btype="checkbox")[^>]*>/.test(html),'full-ledger checkbox exists');
  assert(html.indexOf('<section id="configuration"')<html.indexOf('<section id="simulation"'),
    'settings precede simulation controls');
  assert(/<details\b(?=[^>]*\bid="configuration-panel")(?=[^>]*\bopen(?:\s|>))[^>]*>/.test(html),
    'configuration is expanded on opening the page');

  // Exercise the packaged UI functions. This deliberately models only their DOM
  // boundary; it does not emulate browser layout or reimplement the simulator.
  class TestNode {
    constructor(tag) {
      this.localName=tag; this.tagName=tag.toUpperCase(); this.children=[]; this.ownText=''; this.dataset={}; this.attributes={};
      this.listeners={}; this.parentElement=null; this.style={}; this.open=false; this.invalid=false;
      this.classList={toggle:(name,on)=>{this.attributes['class:'+name]=on;}};
    }
    set textContent(value) {this.ownText=String(value);this.children=[];}
    get textContent() {return this.ownText+this.children.map(child=>child.textContent).join(' ');}
    set innerHTML(value) {throw new Error('compact UI must insert user data as text');}
    set value(value) {this.currentValue=String(value);}
    get value() {return this.currentValue || '';}
    get valueAsNumber() {return Number(this.value);}
    get firstElementChild() {return this.children.find(child=>child.localName!=='#text') || null;}
    get tBodies() {return this.children.filter(child=>child.localName==='tbody');}
    get rows() {return this.children.filter(child=>child.localName==='tr');}
    get cells() {return this.children.filter(child=>child.localName==='td' || child.localName==='th');}
    append(...children) {
      for (let child of children) {
        if (!(child instanceof TestNode)) {const text=new TestNode('#text');text.textContent=child;child=text;}
        child.parentElement=this;this.children.push(child);
      }
    }
    replaceChildren(...children) {this.children=[];this.ownText='';this.append(...children);}
    setAttribute(name,value) {this.attributes[name]=String(value);}
    addEventListener(name,listener) {this.listeners[name]=listener;}
    closest(selector) {
      for (let node=this;node;node=node.parentElement) if(node.localName===selector)return node;
      return null;
    }
    querySelector(selector) {return this.querySelectorAll(selector)[0] || null;}
    querySelectorAll(selector) {
      const all=this.children.flatMap(child=>[child,...child.querySelectorAll('*')]);
      if (selector==='*') return all;
      if (selector===':invalid') return all.filter(node=>node.invalid);
      if (selector==='input:invalid, select:invalid, textarea:invalid') return all.filter(node=>node.invalid && ['input','select','textarea'].includes(node.localName));
      if (selector==='input[data-path]') return all.filter(node=>node.localName==='input' && node.dataset.path);
      if (selector==='details[open]') return all.filter(node=>node.localName==='details' && node.open);
      const path=selector.match(/^\[data-path="([^"]+)"\]$/)?.[1];
      if (path) return all.filter(node=>node.dataset.path===path);
      if (selector==='tbody tr') return all.filter(node=>node.localName==='tr' && node.parentElement.localName==='tbody');
      return all.filter(node=>selector.split(',').map(item=>item.trim()).includes(node.localName));
    }
    reportValidity() {this.reported=true;return !this.invalid;}
    scrollIntoView() {this.scrolled=true;}
  }
  const nodes=new Map();
  const $=id=>{
    if(!nodes.has(id))nodes.set(id,new TestNode('div'));
    return nodes.get(id);
  };
  const form=$('config-form');
  for(const id of ['global-fields','shock-fields','consumers-fields','forecast-fields','assurance-fields','product-fields'])form.append($(id));
  const scenarioTable=$('scenario-table');scenarioTable.localName='table';scenarioTable.tagName='TABLE';scenarioTable.append(new TestNode('tbody'));form.append(scenarioTable);
  const state={result:structuredClone(result),config:null};
  let comparisons=[];
  const context=vm.createContext({Node:TestNode,document:{createElement:tag=>new TestNode(tag),querySelector:selector=>form.querySelector(selector)},$,state,
    clone:value=>JSON.parse(JSON.stringify(value)),updateButtons:()=>{},mutateConfig:()=>{},
    chart:(title,unit,series,zeroLine)=>{
      const figure=new TestNode('figure');figure.chartData={title,unit,series,zeroLine};return figure;
    },
    renderPriceExplanation:(record,config,mode)=>{comparisons.push({record,config,mode});return new TestNode('section');}
  });
  const helpers=vm.runInContext(
    block('  const colors =','  // Strict import helpers begin.')+
    block('  function element(','  function showError(')+
    block('  function inputFor(','  function configChanged(')+
    block('  function table(','  function renderResults(')+
    block('  function renderLedger(','  const svgNS =')+
    block('  function renderCharts(','  // Price explanation helpers begin.')+
    block('  function renderDay(','  function download(')+
    '\n({buildConfig,readConfig,revealInvalidField,renderCharts,renderLedger,renderDay,selectDay,ledgerMetrics});',context);
  helpers.buildConfig(result.config);
  const leaves=(value,path='')=>value!==null && typeof value==='object'
    ? Object.entries(value).flatMap(([key,entry])=>leaves(entry,path?path+'.'+key:key)) : [path];
  const fields=form.querySelectorAll('input[data-path]');
  assert.deepEqual(fields.map(input=>input.dataset.path).sort(),leaves(result.config).sort(),'every configuration value remains editable once');
  assert.deepEqual(JSON.parse(JSON.stringify(helpers.readConfig())),result.config,'collapsed settings preserve exact config round trip');
  const outer=new TestNode('details'),inner=new TestNode('details'),invalid=new TestNode('input');
  outer.append(inner);inner.append(invalid);form.append(outer);invalid.invalid=true;
  assert.equal(helpers.revealInvalidField(form),false,'invalid settings block a run');
  assert(outer.open && inner.open && invalid.reported,'all containing settings open before reporting an invalid field');
  invalid.invalid=false;outer.open=false;inner.open=false;
  assert.equal(helpers.revealInvalidField(form),true);
  assert(!outer.open && !inner.open,'valid settings need not be expanded');

  const charts={};
  for(const view of ['overview','activity','accounts','all']) {
    $('chart-view').value=view;helpers.renderCharts();
    charts[view]=$('charts').children.filter(node=>node.chartData).map(node=>node.chartData);
  }
  assert.equal(charts.overview.length,result.config.products.length+2,'overview contains each price, funding balance and cash');
  assert.equal(charts.activity.length,result.config.products.length+4,'activity contains each stock and four customer/provision measures');
  assert.equal(charts.accounts.length,6,'accounts contains daily funding, both contribution comparisons, result, reserve and arrears');
  const titles=view=>charts[view].map(chart=>chart.title).sort();
  assert.deepEqual(titles('all'),[...titles('overview'),...titles('activity'),...titles('accounts')].sort(),'all charts remains the complete view');
  assert.equal(new Set(titles('all')).size,charts.all.length,'chart views do not duplicate series');
  for(const plotted of charts.all) for(const series of plotted.series) {
    assert(series.points.length>0,'selected chart has points');
    assert(series.points.every(point=>Number.isInteger(point.day) && (point.value===null || Number.isFinite(point.value))),
      'chart filtering preserves numeric observations and missing values');
  }
  const prices=charts.overview.filter(chart=>chart.title.includes('public price'));
  assert.equal(prices.length,result.config.products.length);
  for(const product of result.config.products) {
    const plotted=prices.find(chart=>chart.title.includes('('+product.sku+')'));
    assert(plotted,'each configured product price remains visible');
    for(const [index,mode] of ['optimized','fixed'].entries())
      assert.deepEqual(JSON.parse(JSON.stringify(plotted.series[index].points)),result[mode].rows.map(row=>({day:row.day,value:row.products.find(item=>item.sku===product.sku).selected_price})),
        'price chart uses unchanged '+mode+' observations');
  }

  $('ledger-all').checked=false;helpers.renderLedger();
  let ledger=$('ledger').children[0];
  assert.equal(ledger.children.find(node=>node.localName==='thead').rows[0].cells.length,7,'compact ledger has day, policy and five key fields');
  assert.equal(ledger.tBodies[0].rows.length,result.optimized.rows.length+result.fixed.rows.length,'compact ledger retains every daily record');
  $('ledger-all').checked=true;helpers.renderLedger();ledger=$('ledger').children[0];
  assert.equal(ledger.children.find(node=>node.localName==='thead').rows[0].cells.length,helpers.ledgerMetrics.length+2,'full ledger retains every account column');
  $('selected-day').value='1';
  for(const [selected,expected] of [['optimized',['optimized']],['fixed',['fixed']],['both',['optimized','fixed']]]) {
    $('selected-policy').value=selected;comparisons=[];helpers.renderDay();
    assert.deepEqual(comparisons.map(item=>item.mode),expected,'day inspector follows policy selector');
    for(const pre of $('product-detail').querySelectorAll('pre'))assert(pre.closest('details'),'raw day records stay in expandable details');
  }
  comparisons=[];ledger.tBodies[0].rows[1].cells[0].children[0].listeners.click();
  assert.equal($('selected-policy').value,'fixed','fixed ledger row opens the corresponding policy');
  assert.equal($('selected-day').value,'1');assert($('day-detail').scrolled);
  assert.deepEqual(comparisons.map(item=>item.mode),['fixed']);
  const before=JSON.stringify(state.result);
  $('selected-day').value=String(result.config.periods+1);$('selected-policy').value='fixed';comparisons=[];helpers.renderDay();
  assert.equal(comparisons.length,0,'missing path days do not fabricate an operating record');
  assert($('product-detail').textContent.includes('no operating record'));
  vm.runInContext(block('  $("selected-day").addEventListener','  $("add-product").addEventListener'),context);
  $('selected-day').value='1';$('selected-policy').value='optimized';comparisons=[];
  $('selected-policy').listeners.change();
  assert.deepEqual(comparisons.map(item=>item.mode),['optimized'],'policy control is wired to day inspection');
  $('chart-view').value='activity';$('chart-view').listeners.change();
  assert.equal($('charts').children.filter(node=>node.chartData).length,charts.activity.length,'chart control is wired to chart filtering');
  $('ledger-all').checked=false;$('ledger-all').listeners.change();
  assert.equal($('ledger').children[0].children.find(node=>node.localName==='thead').rows[0].cells.length,7,'ledger control is wired to column selection');
  assert.equal(JSON.stringify(state.result),before,'view changes do not mutate simulation results');
  vm.runInContext(block('  function updateButtons(','  function disposeWorker('),context);
  const savedResult=state.result;state.result=null;
  vm.runInContext('updateButtons();',context);
  for(const id of ['chart-view','selected-policy','ledger-all'])assert($(id).disabled,'empty results disable '+id);
  state.result=savedResult;state.busy=true;
  vm.runInContext('updateButtons();',context);
  assert($('run').disabled && $('step').disabled && !$('stop').disabled,'running simulation disables competing run actions');
  assert(fields.every(input=>input.disabled),'running simulation locks configuration inputs');
  state.busy=false;vm.runInContext('updateButtons();',context);
  for(const id of ['chart-view','selected-policy','ledger-all'])assert(!$(id).disabled,'completed results enable '+id);
  assert(fields.every(input=>!input.disabled),'configuration inputs unlock after completion');

  // Deliver the asynchronous worker boundary explicitly, while using the real
  // request lifecycle, defaults/run/reset actions and result renderers.
  const requests=[];
  class PendingWorker {
    constructor() {this.listeners={};}
    addEventListener(name,listener) {this.listeners[name]=listener;}
    postMessage(message) {requests.push({worker:this,message:structuredClone(message)});}
    terminate() {this.terminated=true;}
  }
  Object.assign(context,{
    Worker:PendingWorker,Blob:class {},URL:{createObjectURL:()=> 'blob:test',revokeObjectURL:()=>{}},
    showError:message=>{$('error').textContent=message;},status:message=>{$('status').textContent=message;}
  });
  $('wasm-runtime').textContent='createExchangeModule';
  $('chart-view').value='overview';$('selected-policy').value='optimized';
  Object.assign(state,{config:null,result:null,runConfig:null,currentDay:0,history:null,manifest:null,
    dirty:false,busy:false,worker:null,sequence:0,pending:null});
  vm.runInContext(block('  function disposeWorker(','  function table(')+
    block('  function renderResults(','  function renderLedger(')+
    block('  $("run").addEventListener','  $("stop").addEventListener'),context);
  const startup=appSource.match(/  updateButtons\(\);\r?\n  defaults\(\);\r?\n\}\)\(\);\s*$/)?.[0];
  assert(startup,'opening the page invokes defaults through the production startup');
  vm.runInContext(startup.slice(0,startup.lastIndexOf('})();')),context);
  const respond=(request,response)=>request.worker.listeners.message({data:{id:request.message.id,...response}});
  const defaultReply={status:'ok',schema_version:'exchange.sim.v3',config:structuredClone(result.config)};
  assert.equal(requests.length,1,'startup waits for the defaults response before simulating');
  assert.equal(requests[0].message.command.op,'defaults');
  assert(state.busy && state.result===null && state.config===null);
  respond(requests[0],{result:defaultReply});
  assert.equal(requests.length,2,'successful defaults hand off to exactly one simulation');
  assert.equal(requests[1].message.command.op,'simulate');
  assert.deepEqual(requests[1].message.command.config,result.config,'automatic run preserves the configured seed, horizon and parameters');
  assert.deepEqual(requests[1].message.command.history,{schema_version:'exchange.history.v1',observations:[]},
    'automatic defaults use an explicit empty history, never fabricated observations');
  assert(state.busy && state.result===null,'no result is applied while the default run is pending');
  respond(requests[1],{result:structuredClone(result)});
  assert(!state.busy && state.currentDay===result.config.periods,'default run reaches the configured horizon');
  assert.deepEqual(JSON.parse(JSON.stringify(state.result)),result);
  assert($('summary').children.length && $('charts').children.length && $('ledger').children.length,
    'automatic completion renders the overview, charts and ledger');
  assert.deepEqual(JSON.parse($('raw-result').textContent),result,'complete default records are inspectable immediately');

  $('reset').listeners.click();
  assert.equal(state.result,null);assert.equal(state.currentDay,0);
  assert.deepEqual(JSON.parse(JSON.stringify(state.config)),result.config,'reset preserves editable default settings');
  assert.equal(requests.length,2,'reset clears results without silently rerunning');
  $('step').listeners.click();
  assert.equal(requests[2].message.command.config.periods,1,'manual step after reset starts at day one');
  assert.equal(requests[2].message.command.config.seed,result.config.seed);
  respond(requests[2],{error:'test worker failure'});
  assert.equal(state.result,null,'failed step does not invent or reuse a result');
  assert.equal(state.currentDay,0);assert(!state.busy);
  assert($('error').textContent.includes('test worker failure'));

  // Restoring the example must discard imported history, then run the defaults
  // only after their successful response, exactly as on initial page load.
  state.history={schema_version:'exchange.history.v1',observations:[{test_record:true}]};
  state.manifest={run_id:'test-import'};state.result=structuredClone(result);
  $('defaults').listeners.click();
  assert.equal(requests[3].message.command.op,'defaults');
  respond(requests[3],{result:defaultReply});
  assert.equal(state.manifest,null);assert.equal(state.result,null);
  assert.equal(requests.length,5);
  assert.deepEqual(requests[4].message.command.config,result.config);
  assert.deepEqual(requests[4].message.command.history,{schema_version:'exchange.history.v1',observations:[]});
  respond(requests[4],{result:{status:'error',error:{message:'test default simulation failure'}}});
  assert.equal(state.result,null,'failed automatic run leaves the cleared results empty');
  assert(!state.busy && $('error').textContent.includes('test default simulation failure'));
  $('defaults').listeners.click();
  respond(requests[5],{error:'test defaults failure'});
  assert.equal(requests.length,6,'failed defaults do not start a simulation');
  assert.equal(state.result,null);assert(!state.busy);
}

function priceExplanationRegressions(appSource, result) {
  const block=(start,end)=>{
    const first=appSource.indexOf(start), last=appSource.indexOf(end,first);
    assert(first>=0 && last>first,'packaged price explanation helpers exist');
    return appSource.slice(first,last);
  };
  // Execute the delivered rendering helpers, with only the DOM boundary faked.
  // No browser, synthetic customer run or alternative pricing implementation is used.
  class TestNode {
    constructor(tag) {this.tagName=tag;this.children=[];this.ownText='';}
    set textContent(value) {this.ownText=String(value);this.children=[];}
    get textContent() {return this.ownText+this.children.map(child=>child.textContent).join(' ');}
    set innerHTML(value) {throw new Error('price explanations must insert user data as text');}
    append(...children) {
      for (const child of children) {
        if (child instanceof TestNode) this.children.push(child);
        else {const text=new TestNode('#text');text.textContent=child;this.children.push(text);}
      }
    }
  }
  const render=vm.runInNewContext(
    block('  const numberFormat =','  const globalFields =')+
    block('  function element(','  function showError(')+
    block('  function table(','  const summaryMetrics =')+
    block('  // Price explanation helpers begin.','  // Price explanation helpers end.')+
    '\nrenderPriceExplanation;', {Node:TestNode,document:{createElement:tag=>new TestNode(tag)}});
  const descendants=node=>[node,...node.children.flatMap(descendants)];
  const config=result.config, row=result.optimized.rows[0];
  const rendered=render(row,config,'optimized');
  assert(rendered.textContent.includes('Why this price?'));
  assert(rendered.textContent.includes('Published — optimizer recommendation.'));
  assert(rendered.textContent.includes('keeps every other product at its published price'));
  assert(rendered.textContent.includes('not divided into invented per-product charges'));
  assert(rendered.textContent.includes('operating balance price direction violated for bread'),
    'actual neutral-day candidate exclusions appear in the explanation');
  const requirements=descendants(rendered).find(node=>node.tagName==='table' &&
    node.children.some(child=>child.tagName==='caption' && child.textContent==='What this day’s prices are intended to fund'));
  assert(requirements,'whole-exchange requirements table exists');
  const values=new Map(requirements.children.find(node=>node.tagName==='tbody').children.map(tr=>
    [tr.children[0].textContent,Number(tr.children[1].textContent.replaceAll(',',''))]));
  const need=row.worker_wages_due+row.operating_cost_due+row.reserve_requirement;
  assert.equal(values.get('Total requirement before feedback'),need);
  assert.equal(values.get('Contribution target: total requirement − daily adjustment'),need-row.feedback_adjustment);
  assert.equal(descendants(rendered).filter(node=>node.tagName==='h5').length,row.products.length,
    'every product is represented separately without dividing the shared requirement');
  const rankedRow=result.optimized.rows.find(day=>day.status==='recommended' && day.products.some(product=>
    product.candidate_forecasts.some(candidate=>candidate.price_comparison.admissible && !candidate.price_comparison.published)));
  assert(rankedRow,'default run exercises a permitted alternative as well as exclusions');
  const ranked=render(rankedRow,config,'optimized');
  const alternatives=rankedRow.products.flatMap(product=>product.candidate_forecasts.map(candidate=>candidate.price_comparison))
    .filter(comparison=>comparison.admissible && !comparison.published);
  if (alternatives.some(comparison=>comparison.hypothetical_balance_score>rankedRow.expected_absolute_balance))
    assert(ranked.textContent.includes('Its balance distance is greater than the published combination’s.'));
  if (alternatives.some(comparison=>comparison.hypothetical_balance_score===rankedRow.expected_absolute_balance))
    assert(ranked.textContent.includes('It has the same reported balance distance as the published combination.'));
  assert(!ranked.textContent.includes('Diagnostic discrepancy:'),'published optimum has no apparently better permitted one-product alternative');

  // Hand-authored presentation fixture for the documented 160/200-cent example.
  // It verifies the distinction between a smaller score and a permitted choice;
  // the C++ tests independently verify how diagnostics are calculated.
  const guardedRow={...row,worker_wages_due:400,operating_cost_due:0,reserve_requirement:0,feedback_adjustment:1000,
    products:[{...row.products[0],label:'<img src=x onerror=alert(1)>',sku:'bread',unit_cost:100,previous_price:200,selected_price:160,
      candidate_forecasts:[
        {price:160,saleable_scenario_units:[30],price_comparison:{expected_units:30,expected_contribution:1800,
          scenario_contribution:[1800],scenario_funding_balance:[2400],expected_funding_balance:2400,hypothetical_balance_score:2400,
          minimum_coverage_slack:1400,admissible:true,exclusion_reasons:[],affordable_alternative_price:null,published:true}},
        {price:200,saleable_scenario_units:[10],price_comparison:{expected_units:10,expected_contribution:1000,
          scenario_contribution:[1000],scenario_funding_balance:[1600],expected_funding_balance:1600,hypothetical_balance_score:1600,
          minimum_coverage_slack:600,admissible:false,exclusion_reasons:['affordable alternative 0 preserves scenario provision and contribution for bread'],
          affordable_alternative_price:160,published:false}}
      ]}]};
  guardedRow.coverage_credit=0; guardedRow.liquidity_buffer=0;
  const guarded=render(guardedRow,{...config,scenarios:[{id:'expected',probability:1}]},'optimized');
  assert(guarded.textContent.includes('Affordable-offer check:'));
  assert(guarded.textContent.includes('160-cent alternative'));
  assert(guarded.textContent.includes('affordable alternative 0 preserves scenario provision and contribution for bread'));
  assert(guarded.textContent.includes('2,400') && guarded.textContent.includes('1,600'),'both hypothetical scores remain visible');
  assert(!guarded.textContent.includes('Diagnostic discrepancy:'),'an excluded cheaper-distance candidate is not presented as a solver error');
  assert(guarded.textContent.includes('<img src=x onerror=alert(1)>') && !descendants(guarded).some(node=>node.tagName==='img'),
    'configured product labels remain literal text');
  const continuityRow={...guardedRow,status:'continuity',expected_absolute_balance:null,worker_wages_due:1000,
    feedback_adjustment:0,coverage_credit:0,liquidity_buffer:100,products:[{...guardedRow.products[0],selected_price:200,
      candidate_forecasts:[{price:200,saleable_scenario_units:[0],price_comparison:{expected_units:0,expected_contribution:0,
        scenario_contribution:[0],scenario_funding_balance:[-1000],expected_funding_balance:-1000,hypothetical_balance_score:1000,
        minimum_coverage_slack:-900,admissible:false,exclusion_reasons:['wages, operating costs, and reserve not covered in scenario expected'],
        affordable_alternative_price:null,published:true}}]}]};
  const continuity=render(continuityRow,{...config,scenarios:[{id:'expected',probability:1}]},'optimized');
  assert(continuity.textContent.includes('Published — continuity hold; not an optimizer recommendation.'));
  assert(continuity.textContent.includes('A request does not create funding'));
  assert(continuity.textContent.includes('All balance distances below are hypothetical comparisons.'));
  assert(!continuity.textContent.includes('Published — optimizer recommendation.'));
  assert(!continuity.textContent.includes('than the published combination') && !continuity.textContent.includes('same reported balance distance'),
    'continuity makes no claim about winning the optimization');
  const fixed=render(result.fixed.rows[0],config,'fixed');
  assert(fixed.textContent.includes('Published — configured fixed price.'));
  assert(!fixed.textContent.includes('Published — optimizer recommendation.'));
  assert(!fixed.textContent.includes('than the published combination') && !fixed.textContent.includes('same reported balance distance'),
    'fixed prices make no claim about winning the optimization');
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
