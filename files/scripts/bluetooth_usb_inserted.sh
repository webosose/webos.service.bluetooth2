#!/bin/sh

systemctl stop hciuart

rmmod hci_uart

systemctl restart webos-bluetooth-service
