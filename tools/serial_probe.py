import serial

PORT = 'COM3'
BAUD = 115200

ser = serial.Serial(PORT, BAUD, timeout=1)

ser.write(b'id\r')

for _ in range(10):
    line = ser.readline()
    if not line:
        break
    print(repr(line))

ser.close()