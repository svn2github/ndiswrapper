DISTFILES= Makefile \
  compat \
  README.Debian \
  control.modules \
  control.utils \
  copyright \
  dirs.utils \
  docs \
  ndiswrapper-utils.install \
  ndiswrapper.install \
  ndiswrapper.substvars \
  postinst.modules \
  rules

all:
	@echo "Run 'make deb' from top-level directory"
	@exit 1

dist:
	@ if [ -z $(distdir) ]; then \
		echo "Incorrect use of make here"; \
		exit 1; \
	fi
	@for file in $(DISTFILES); do \
	  cp  $$file $(distdir)/$$file; \
	done

.PHONY: dist
