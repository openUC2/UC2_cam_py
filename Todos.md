
# Todos and next steps
- Currently MIT Licensed -> should that stay?
- Logging
    - Set up package for proper logging (using python logging and feed log functions in c - parts -> done via import)
    - replace log message
- Triggering
    - implement trigger option for the camera
- Ring Buffering
    - Implement Ring buffer
- Connect everything to newswitch

- Review units of exposure time and gain
    - Gain in dB, Exp time in us?

- Test for different plattforms (ARM, IOS)

- Threading
    - currently "waitForFrame" blocks the caling thread (and hence the python caller!) -> this should be revised -> maybe make it run in thread of the ring buffer later!
    - Pin reading thread to core