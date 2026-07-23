# Scope
Connect to exactly one HIK Cam, and acquire images
# Required features

- Init()
    - inits HIK Cam and returns the number of connected devices
    - Use parameters to filter devices

- Connect(int nr)
    - Connect to the respective device
- Disconnect()
    - Disconnects the device

- Start/Stop

- Snap
- Pointer to image data (including img size)
- Settings:
    - Gain
    - Exposure time
- Kick out log messages


