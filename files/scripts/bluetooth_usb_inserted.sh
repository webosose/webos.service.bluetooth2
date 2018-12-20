#!/bin/sh

systemctl stop brcm43438

rmmod hci_uart

systemctl restart webos-bluetooth-service
