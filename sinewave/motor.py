from pymodbus.client import ModbusSerialClient
from pymodbus.exceptions import ModbusException
import numpy as np
import math

CMD_RESP_US = 50000
CMD_BYTE_US = 50000
FAST_RESP_US = 3000
FAST_BYTE_US = 2000
RAMP_TIME_S = 3.0
DRIVE_PREFLIGHT_READS = 3
DRIVE_PREFLIGHT_ATTEMPTS = 30
ENCODER_CAL_POSITION_LOSS_S = 2.0
RECIP_ENCODER_REVERSE_DELAY_S = 0.020
RECIP_ENCODER_ORIGIN_TIMEOUT_S = 3.0


   
#Modbus client params
MODBUS_PORT=f"COM{5}"
MODBUS_SLAVE_ID=1
MODBUS_BAUD_RATE=19200#115200
MODBUS_PARITY="E"
MODBUS_STOPBITS=1
MODBUS_BYTESIZE=8

#Command values
START_CMD = 0x0001
STOP_CMD = 0x0000


# Controller memory addresses
ADDR_SPEED_VAL = 0x0600+3  # 1540-1 : RPM setpoint (int16)
ADDR_CTRL_MODE = 0x0200+0  # control mode (speed/pos/torque)
ADDR_DI1_FN = 0x0300+2  # DI1 func (disable)
ADDR_SPEED_SRC_A = 0x0600+0  # main speed source A
ADDR_SPEED_SRC_B = 0x0600+1  # aux speed source B
ADDR_SPEED_SRC_SEL = 0x0600+2  # speed command source
ADDR_VDI_ENABLE = 0x0C00+9  # comm VDI enable
ADDR_VDI_LEVEL = 0x3100+0  # VDI virtual
ADDR_MULTI_SPEED_HEAD=0x1200+0
ADDR_MULTI_SPEED_FIRST=0x1200+20
ADDR_DI_FLAGS=0x0300

ADDR_ANALOG_OFFSET=0x0300+36
ADDR_ANALOG_TIME_CTE=0x0300+37
ADDR_ANALOG_DEADZONE=0x0300+39
ADDR_ANALOG_DRIFT=0x0300+40
ADDR_ANALOG_SPEED_10V=0x0300+51

ADDR_POS_SRC_SEL=0x0500+0
ADDR_MULTI_POS_HEAD=0x1100+0
ADDR_MULTI_POS_FIRST=0x1100+12
ADDR_PULSE_PER_ROT=0x0500+2

ADDR_VDI_FN1=0X1700+0
ADDR_VDI_FN2=0X1700+2

ADDR_POS_LOW_PASS=0x0500+4
ADDR_POS_TIME_CTE=0X0500+6

REG_P0B_07 = 0x0B07  # P0B-07 (abs pos, 32-bit) - historical naming
REG_P0B_09 = 0x0B09  # P0B-09 (pos per revolution, 0..65535)
REG_P0B_00 = 0x0B00  # P0B-00 (actual motor speed, rpm)
REG_P0B_33 = 0x0B21  # P0B-33 (fault selector/history index) BUG:WRONG ADDRESS
REG_P0B_34 = 0x0B22  # P0B-34 (fault code selected by P0B-33)BUG:WRONG ADDRESS
REG_P05_02 = 0x0502  # P05-02 (command units per revolution)
REG_P0C_26 = 0x0C26  # word order



class Motor:
    def __init__(self, port=MODBUS_PORT, baudrate=MODBUS_BAUD_RATE,
                 parity=MODBUS_PARITY, slave_id=MODBUS_SLAVE_ID) -> None:
        # INTEGRACAO: somente comunicacao configuravel. A curva e os registros
        # permanecem neste modulo do colega. Importar o arquivo nao conecta.
        self.slave_id = int(slave_id)
        self.client = None
        
        try:
            self.client = ModbusSerialClient(
               port=port,
                baudrate=baudrate,
                parity=parity,
                stopbits=MODBUS_STOPBITS,
                bytesize=MODBUS_BYTESIZE,
                timeout=0.05,
                retries=0
            )
            if not self.client.connect():
                raise RuntimeError("Nao foi possivel abrir a porta do Drive")
        except Exception as e:
            print(f"CONNECTION ERROR:::{e}")
            raise RuntimeError(f"CONNECTION ERROR: {e}") from e

    def start(self):
        print(":::::STARTING MOTOR::::::")
        self._write(ADDR_VDI_LEVEL, 0b11)
    
    def stop(self):
        print(":::::STOPING MOTOR::::::")
        # INTEGRACAO: STOP rejeitado nao pode ser tratado como sucesso.
        # O CH3 verifica repouso fisico; o readback confirma o comando VDI.
        last_error = None
        for _ in range(3):
            try:
                self._write(ADDR_VDI_LEVEL, 0)
                if self._read(ADDR_VDI_LEVEL) != 0:
                    raise RuntimeError('STOP VDI divergiu no readback')
                return
            except Exception as exc:
                last_error = exc
        raise RuntimeError(f'STOP nao confirmado: {last_error}')
    
    def __del__(self):
        self.close()

    def close(self):
        # Fechar a COM nao substitui STOP. O runtime chama stop em finally
        # antes de fechar e registra separadamente falhas de parada.
        if self.client is not None:
            self.client.close()

    def _write(self,addr:int,value:int):
        r=self.client.write_register(addr,value,device_id=self.slave_id)
        if r.isError():
            print(r)
            raise RuntimeError("MODBUS WRITE FAILED")

    def _write_u32(self,addr:int,value:int):
        assert(np.abs(value)<(1<<31)),"POSITION TOO BIG"
        b_high=(value >> 16)& 0xFFFF
        b_low=value & 0xFFFF
        #print(b_high,b_low)
        r=self.client.write_registers(addr,[b_low,b_high],device_id=self.slave_id)
        if r.isError():
            print(r)
            raise RuntimeError("MODBUS WRITE FAILED")
    

    def _read(self,addr:int):
        r=self.client.read_holding_registers(addr,device_id=self.slave_id)
        if r.isError(): 
            print(r)
            raise RuntimeError("MODBUS READ FAILED")
        else:
            value=r.registers[0]
            return value

    def _read_many(self,addr:int,count:int):
        r=self.client.read_holding_registers(addr,count=count,device_id=self.slave_id)
        if r.isError(): 
            print(r)
            raise RuntimeError("MODBUS READ FAILED")
        else:
            return r.registers

#NOTE: THIS THROWS ERROR
    def analog_mode(self): 
        print(":::::SWITCHING TO ANALOG INPUT::::::")
        #NOTE:  I dont know what any of these do, keep it just to be safe.
        self.client.write_register(ADDR_VDI_LEVEL, 0 )  
        self.client.write_register(ADDR_VDI_ENABLE, 1 ) 
        self.client.write_register(ADDR_DI1_FN, 0 )
    
        # Set mode to speed
        self.client.write_register(ADDR_CTRL_MODE, 0 )
        

        #sets speed command to analog A
        data=[1,2,0]
        self.client.write_registers(ADDR_SPEED_SRC_A,data)
    
        #set Analog input params 
        self.client.write_register(ADDR_ANALOG_OFFSET,0)

        self.client.write_register(ADDR_ANALOG_TIME_CTE,0)
        self.client.write_register(ADDR_ANALOG_DEADZONE,0)
        self.client.write_register(ADDR_ANALOG_DRIFT,0)
        self.client.write_register(ADDR_ANALOG_DRIFT,0)
        self.client.write_register(ADDR_ANALOG_SPEED_10V,500)
    


    def pos_ctrl(self,f,times):
        assert(len(times)<=16),"TIMES ARRAY CANT BE BIGGER THAN 16 ELEMENTS"
        print(":::::SETTING POSITION SINE::::::")
        PULSE_PER_ROT=10000
    #NOTE:  I dont know what any of these do, keep it just to be safe.
        self.client.write_register(ADDR_VDI_LEVEL, 0 )  
        self.client.write_register(ADDR_VDI_ENABLE, 1 ) 
        self.client.write_register(ADDR_DI1_FN, 0 )

        self.client.write_register(ADDR_VDI_FN1, 1 )
        self.client.write_register(ADDR_VDI_FN2, 28 )

    
    # Set mode to position
        self.client.write_register(ADDR_CTRL_MODE, 1 )
       
        self.client.write_register(ADDR_POS_SRC_SEL,2) #muti segment
        self.client.write_register(ADDR_PULSE_PER_ROT, 500)#PULSE_PER_ROT )
        self.client.write_register(ADDR_POS_LOW_PASS, 10)
        self.client.write_register(ADDR_POS_TIME_CTE, 10)


        n_segments=len(times)

        TIME_UNIT=0
        data=[1,n_segments,0,TIME_UNIT,0,1]  #header  
        self.client.write_registers(ADDR_MULTI_POS_HEAD,data)

        data=[]
        t=0
        x0=f(times[-1])
        addr=ADDR_MULTI_POS_FIRST
        for dt in times:
            t=t+dt
            raw=f(t)#voltas
            x1=int(np.floor(PULSE_PER_ROT*raw))
            self._write_u32(addr,x1)
            addr=addr+2
            speed=int((x1-x0)/dt)&0xFFFF
            print(x0,x1,speed)
            x0=x1
            self._write(addr,speed)
            addr=addr+1
            accel=int(500*dt)#TODO:DO CALC ON THIS
            self._write(addr,accel)
            addr=addr+1
            wait=0
            self._write(addr,wait)
            addr=addr+1

        read=self._read_many(ADDR_MULTI_POS_FIRST,5*len(times))
        #print(read)
        #for a,b in zip(data,read):
            #assert(a==b),f"VALUE WRITE ERROR: TRIED TO WRITE {a}, READ BACK IS {b}"

 
#NOTE: THIS THROWS ERROR
    def vel_ctrl(self,f,times):
        assert(len(times)<=16),"TIMES ARRAY CANT BE BIGGER THAN 16 ELEMENTS"
        print(":::::SETTING SPEED SINE::::::")
        # INTEGRACAO: somente este caminho usado pelo adaptador teve as
        # respostas do setup verificadas. O algoritmo abaixo nao foi trocado.
        self._write(ADDR_VDI_LEVEL, 0)
        self._write(ADDR_VDI_ENABLE, 1)
        self._write(ADDR_DI1_FN, 0)
    
        # Set mode to speed
        self._write(ADDR_CTRL_MODE, 0)
        

        #sets speed command to multi segment
        data=[0,5,1]
        response = self.client.write_registers(ADDR_SPEED_SRC_A,data,device_id=self.slave_id)
        if response.isError() or self._read_many(ADDR_SPEED_SRC_A,3) != data:
            raise RuntimeError('Fontes de velocidade rejeitadas/divergentes')
        #set multi speed params
        
        n_segments=len(times)

        data=[1,n_segments,0]  #header 
        response = self.client.write_registers(ADDR_MULTI_SPEED_HEAD,data,device_id=self.slave_id)
        if response.isError() or self._read_many(ADDR_MULTI_SPEED_HEAD,3) != data:
            raise RuntimeError('Cabecalho de segmentos rejeitado/divergente')
    

        data=[]
        t=0
        for dt in times:
            t=t+dt
            speed=int(np.floor(f(t)))
            if speed<0:
                speed=speed & 0xFFFF
            print(speed)
            data.append(speed)
            data.append(int(dt))
            data.append(0)

        #self._write_many(ADDR_MULTI_SPEED_FIRST,data)
        for i,d in enumerate(data):
            self._write(ADDR_MULTI_SPEED_FIRST+i,d)
            v=self._read(ADDR_MULTI_SPEED_FIRST+i)
            assert(d==v),f"VALUE WRITE ERROR: TRIED TO WRITE {d}, READ BACK IS {v}"
        #self.client.write_registers(ADDR_MULTI_SPEED_FIRST,data)


def try_loop(func):
    N_TRIES=5
    while N_TRIES:
        try:
            return func()
        except Exception as e:
            print(f"THREW Exception {e}, retrying operation")
            N_TRIES=N_TRIES-1



#NOTE:  MAIN SCRIPT
# INTEGRACAO: exemplo manual isolado; importar Motor nao abre COM nem envia STOP.
if __name__ == "__main__":
     
    
    NUM_SEGMENTS=16
    SEG_TIME=1
    times = np.ones(NUM_SEGMENTS)*SEG_TIME  #np.linspace(0, 2 * np.pi, NUM_SEGMENTS)
    period=np.sum(times)
    motor=Motor()    
    
    freq=1/(period)
    print("freq:",freq)
    MAX_VAL=50
    
    def test(i):
        freq=16
        v=math.fmod(i,freq)/freq
        if v>=0.5:
            return -MAX_VAL
        else:
            return MAX_VAL
    
    f_sine=lambda i: 100*MAX_VAL*np.sin(freq*i)
    
    r_sine=lambda i: 0.1*np.sin(freq*i)
    
    try_loop(lambda:motor.stop())
    
    #motor.vel_ctrl(test,times)
    #motor.vel_ctrl(f_sine,times)
    #motor.pos_ctrl(r_sine,times)
    
    #try_loop(lambda:motor.start())
    
       
