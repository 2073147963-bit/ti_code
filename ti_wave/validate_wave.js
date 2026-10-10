const {join}=require('path');
const fs=require('fs');
const ds=initScripting(); ds.setScriptingTimeout(20000);
let s;
const results=[];
try {
 ds.configure(join(__dirname,'targetConfigs/MSPM0G3507.ccxml'));
 s=ds.openSession(/cortex/i); s.target.connect();
 const cases=[['sine100',100,0],['sine200',200,0],['sine400',400,0],['square200',200,1],['dc',0,-2],['rail',0,-3],['captured',0,-3]];
 for(const [name,f,expected] of cases) {
  s.target.halt(); s.memory.loadProgram(join(__dirname,'Debug/ti_wave.out'));
  const bp=s.breakpoints.add('wave_analyze'); s.target.run(); s.breakpoints.remove(bp);
  const data=name==='captured'?JSON.parse(fs.readFileSync(join(__dirname,'adc_capture.json'),'utf8')):
   Array.from({length:10000},(_,i)=> name==='rail'?4095:name==='dc'?2048:
    Math.round(2048+620*(name.startsWith('square')?(Math.sin(2*Math.PI*f*i/10000)>=0?1:-1):Math.sin(2*Math.PI*f*i/10000))));
  const addr=s.registers.read('R0'), out=s.registers.read('R2');
  s.memory.write(addr,data,16);
  const ret=s.breakpoints.add(BigInt(s.registers.read('LR')) & ~1n);
  s.target.run(); s.breakpoints.remove(ret);
  const rc=Number(BigInt.asIntN(32,BigInt(s.registers.read('R0'))));
  const words=s.memory.read(out,4,32).map(Number);
  const buf=Buffer.alloc(16); words.forEach((v,i)=>buf.writeUInt32LE(v>>>0,i*4));
  const result={name,rc,type:words[0],freq:buf.readFloatLE(4),vpp:buf.readFloatLE(8),pass:expected<0?rc===expected:rc===0&&words[0]===expected&&Math.abs(buf.readFloatLE(4)-f)<1};
  results.push(result); console.log(JSON.stringify(result));
 }
 fs.writeFileSync(join(__dirname,'algorithm_validation.json'),JSON.stringify(results,null,2));
} finally {
 if(s){s.target.halt();s.breakpoints.removeAll();s.memory.loadProgram(join(__dirname,'Debug/ti_wave.out'));s.target.run(false);s.target.disconnect();}
 ds.shutdown();
}
if(results.some(r=>!r.pass)) throw new Error('Algorithm validation failed');
