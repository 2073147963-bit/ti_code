// Run with D:/AppGallery/ccs/ccs/scripting/run.bat flash_wave.js
const {join} = require('path');
const ds = initScripting();
ds.setScriptingTimeout(20000);
try {
  ds.configure(join(__dirname, 'targetConfigs/MSPM0G3507.ccxml'));
  const session = ds.openSession(/cortex/i);
  session.target.connect();
  session.target.halt();
  session.memory.loadProgram(join(__dirname, 'Debug/ti_wave.out'));
  session.target.run(false);
  console.log('Firmware loaded and running.');
  sleep(1000);
  if (session.target.isHalted()) throw new Error('Target halted unexpectedly');
  session.target.disconnect();
  console.log('Detached; firmware continues running. Verify output in log_wave.ps1.');
} finally { ds.shutdown(); }
