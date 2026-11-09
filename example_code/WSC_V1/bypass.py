import sys
import time
import threading

# [1] Skip System Clock Configuration (Hook)
def skip_function(cpu, addr):
    # Log message changed to English to prevent encoding errors
    cpu.Log(LogLevel.Info, '[Renode] Skipping SystemClock_Config (Success!)')
    cpu.PC = cpu.LR

print "--------------------------------------------------"
print "Initializing Python Bypass Script..."

# Try to install the hook
try:
    # 1. Find address of SystemClock_Config
    func_addr = sysbus.GetSymbolAddress('SystemClock_Config')
    
    # 2. Get the CPU
    cpu = sysbus.GetMachine().GetLocalPeripheralsByName('cpu')[0]
    
    # 3. Add the hook
    cpu.AddHook(func_addr, skip_function)
    print "HOOK INSTALLED on SystemClock_Config!"
except:
    print "Warning: Could not find SystemClock_Config symbol."

print "--------------------------------------------------"

# [2] Traffic Generator (Auto Input)
def traffic_generator():
    time.sleep(3) # Wait for boot
    while True:
        try:
            # Send 'A' to UART1
            uart_list = sysbus.GetMachine().GetLocalPeripheralsByName('usart1')
            if len(uart_list) > 0:
                uart_list[0].WriteChar(0x41)
                print '[Auto-Gen] Sent character A to UART1'
            
            time.sleep(2)
        except:
            break

# Start background thread
t = threading.Thread(target=traffic_generator)
t.daemon = True
t.start()
