#!/bin/sh
echo insert hci_uart
insmod /lib/modules/`uname -r`/kernel/drivers/bluetooth/hci_uart.ko
sleep 2
systemctl start hciuart

systemctl restart webos-bluetooth-service
