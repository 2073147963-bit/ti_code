const {join} = require('path');
const fs = require('fs');
const ds = initScripting();
ds.setScriptingTimeout(20000);
let session;
try {
 ds.configure(join(__dirname,'targetConfigs/MSPM0G3507.ccxml'));
 session=ds.openSession(/cortex/i);
 session.target.connect(); session.target.halt();
 session.memory.loadProgram(join(__dirname,'Debug/ti_wave.out'));
 const bp=session.breakpoints.add('wave_analyze');
 session.target.run();
 const addr=session.registers.read('R0');
 const n=Number(session.registers.read('R1'));
 const data=session.memory.read(addr,n,16).map(Number);
 fs.writeFileSync(join(__dirname,'adc_capture.json'),JSON.stringify(data));
 console.log(JSON.stringify({address:String(addr),n,min:Math.min(...data),max:Math.max(...data),first:data.slice(0,80)}));
 session.breakpoints.remove(bp);
 session.target.run(false); session.target.disconnect();
} finally { ds.shutdown(); }
