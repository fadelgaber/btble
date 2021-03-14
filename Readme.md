An example to communicate with DATS server over Bluetooth

paulvha / November 2020 / Version 1.0

# Ambiq SDK/Apollo3

Install and compile the dats server as described.

# Client side

## Ubuntu and Raspberry Pi

### Install Bluez 5.55

MUST BE Bluez 5.55

Follow the instruction for Bluez installation and compilation on:
https://www.jaredwolff.com/get-started-with-bluetooth-low-energy
P.s. there are many other sites that provide guidance as well.

For Raspberry Pi there are extended instructions, but the one above should do
https://learn.adafruit.com/install-bluez-on-the-raspberry-pi/installation

### Install client
Once you have installed and compiled Bluez, then :

cd bluez-5.55
copy the 'btble'-directory as a directory in this bluez directory
cd btble

optional : update permissions : chmod +x make_BTBLE
compile : ./make_btble

### execute client example

./btble -b 56:77:88:23:AB:EF (with the correct destination address)

or ./btble --help   for more options / help information

P.s. to get an BLuetooth address run : hcitool lescan
