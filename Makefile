CC=gcc
.PHONY: install uninstall test
user = $(shell whoami)

all:
	@echo "Building zmodopipe binary"
	$(CC) -Wall zmodopipe.c -o zmodopipe
	@echo "\nTo install dvralarm run the following command"
	@echo "sudo make install"

install:
	$(if $(filter $(user),root),@echo "Installing dvralarm",@echo "WARNING not root!\nTrying to install dvralarm")
	cp ./dvralarm_pi.py /usr/local/bin
	cp ./dvralarm.sh /etc/init.d
	cp ./zmodopipe /usr/bin
	chmod 755 /usr/local/bin/dvralarm_pi.py
	chmod 755 /etc/init.d/dvralarm.sh
	chmod 755 /usr/bin/zmodopipe
	update-rc.d dvralarm.sh defaults
	/usr/local/bin/dvralarm_pi.py -i
	/etc/init.d/dvralarm.sh start
	@echo "\n## Install completed\nManage dvralarm service"
	@echo "sudo /etc/init.d/dvralarm.sh start|stop|status"
	@echo "\nTo uninstall run the following command"
	@echo "sudo make uninstall"
	@echo "\nTo configure please edit /etc/dvralarm/config.json directly"

uninstall:
	$(if $(filter $(user),root),@echo "Uninstalling dvralarm",@echo "WARNING not root!\nTrying to uninstall dvralarm")
	/etc/init.d/dvralarm.sh stop
	sudo update-rc.d -f dvralarm.sh remove
	rm /usr/local/bin/dvralarm_pi.py
	rm /etc/init.d/dvralarm.sh
	rm /usr/bin/zmodopipe
	@echo "\n## Uninstall completed"

test:
	@echo "For testing purposes"
	
