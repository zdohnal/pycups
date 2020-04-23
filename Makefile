PYTHON=python
NAME=pycups
VERSION:=$(shell $(PYTHON) setup.py --version)
SDIST_ARGS=--formats=gztar -d.
RPMCONFIGDIR:=$(shell rpm -E "%{_rpmconfigdir}" 2>/dev/null || :)

SOURCES=cupsmodule.c cupsconnection.c cupsppd.c cupsipp.c setup.py \
	cupsppd.h cupsipp.h cupsconnection.h cupsmodule.h \
	psdriver.attr postscriptdriver.prov

DIST=Makefile test.py \
	examples \
	COPYING NEWS README TODO

cups.so: force
	$(PYTHON) setup.py build
	ln -sf build/lib*/$@ .

doc:	cups.so
	rm -rf html
	epydoc -o html --html $<

doczip:	doc
	cd html && zip ../cups-html.zip *

clean:
	-rm -rf build cups.so *.pyc *~

dist:
	$(PYTHON) setup.py sdist $(SDIST_ARGS)

upload:
	$(PYTHON) setup.py sdist $(SDIST_ARGS) upload -s

install:	install-rpmhook
	ROOT= ; \
	if [ -n "$$DESTDIR" ]; then ROOT="--root $$DESTDIR"; fi; \
	$(PYTHON) setup.py install --skip-build $$ROOT

install-rpmhook:
	if [ -n "$(RPMCONFIGDIR)" ]; then \
		RPMCONFIG="$$DESTDIR$(RPMCONFIGDIR)" ; \
		mkdir -p "$$RPMCONFIG"/fileattrs ; \
		install -m0644 psdriver.attr "$$RPMCONFIG"/fileattrs/ ; \
		install -m0755 postscriptdriver.prov "$$RPMCONFIG"/ ; \
	fi

.PHONY: doc doczip clean dist install install-rpmhook force
