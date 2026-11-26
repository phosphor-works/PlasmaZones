# SPDX-FileCopyrightText: 2026 Phosphor Works Team
# COPR SRPM build instructions
# Called by COPR build system: make -f .copr/Makefile srpm outdir=<path>
# Also usable locally: make -f .copr/Makefile srpm outdir=./results
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Note: the COPR SRPM mock chroot is minimal (no git). Use tar, not git archive.
# git is installed on demand to fetch the recipe and to read the revision tag,
# see the srpm recipe.
#
# The spec and its changelog generator are in plasmazones/ of the packaging
# repository, not in this tree. The recipe clones that repository and copies
# plasmazones/ to packaging/ here, the same place the release workflow puts it,
# and replaces any packaging/ that is already there. COPR's make_srpm method
# runs with network access.

PACKAGING_URL := https://github.com/phosphor-works/packaging
SPEC := packaging/rpm/plasmazones.spec
NAME := plasmazones

# Extract version from CMakeLists.txt (spec has placeholder 0.0.0; CI stamps it)
VERSION := $(shell sed -n 's/^project(PlasmaZones VERSION \([^ )]*\).*/\1/p' CMakeLists.txt)
# An empty parse would write "Version:" with no value into the spec and tar to
# plasmazones-.tar.gz. Both other consumers of this parse guard it the same way.
ifeq ($(strip $(VERSION)),)
$(error Could not parse VERSION from CMakeLists.txt)
endif
# VERSION as a sed pattern, so 3.5.3 cannot match a tag like v3x5x3-r2.
VERSION_RE := $(subst .,\.,$(VERSION))

srpm:
	@echo "=== Building SRPM for $(NAME)-$(VERSION) ==="

	# Fetch the recipe from the packaging repository.
	command -v git >/dev/null || dnf -y install git-core
	rm -rf /tmp/$(NAME)-packaging packaging
	git clone --depth 1 $(PACKAGING_URL) /tmp/$(NAME)-packaging
	mkdir -p packaging
	cp -a /tmp/$(NAME)-packaging/plasmazones/. packaging/

	# Stamp version into spec
	sed -i 's/^Version:.*/Version:        $(VERSION)/' $(SPEC)

	# Stamp the package revision into the spec. A rebuild-only republish tags
	# the release's commit again as v<version>-r<N> (plasmazones-rebuild-watch.yml
	# of the packaging repository),
	# and COPR builds that tag from the same source, so the highest -r<N> tag
	# at HEAD is the revision and a bare v<version> is revision 1. Without it
	# every rebuild came out as <version>-1, dnf saw the NEVR already installed
	# and never offered the rebuild against the new KWin. git was installed
	# above, and the recipe fails rather than fall back to 1 when the tags
	# cannot be read, since that fallback is the bug.
	tags="$$(git tag --points-at HEAD)" || exit 1; \
	pkgrel="$$(printf '%s\n' "$$tags" | sed -n 's/^v$(VERSION_RE)-r\([1-9][0-9]*\)$$/\1/p' | sort -n | tail -n 1)"; \
	pkgrel="$${pkgrel:-1}"; \
	echo "Package revision: $$pkgrel"; \
	sed -i "s/^Release:.*/Release:        $$pkgrel%{?dist}/" $(SPEC); \
	if [ -x packaging/generate-changelog.sh ]; then \
		bash packaging/generate-changelog.sh rpm "" "$$pkgrel"; \
	fi

	# Create source tarball matching Source0 pattern in the spec
	# (no git in mock chroot — use tar with --transform to set prefix)
	# .cache holds CPM's downloads from a local configure, and packaging/ the
	# recipe copied in above: neither belongs in the RPM's Source0, which is
	# the tree of the tag, as the GitHub archive of it is.
	mkdir -p /tmp/$(NAME)-srpm
	tar czf /tmp/$(NAME)-srpm/$(NAME)-$(VERSION).tar.gz \
		--transform 's,^\.,PlasmaZones-$(VERSION),' \
		--exclude=.git --exclude='build*' --exclude=.claude \
		--exclude=./.cache --exclude=./packaging .

	# The spec's Source1 to Source4, the dependency archives pinned in
	# cmake/PlasmaZonesDependencies.cmake and the pinned phosphor-libs tree. rpmbuild -bs packs every Source into
	# the SRPM and fails on a missing one, and the binary build then reads them
	# offline from its source directory. The script mode downloads each one
	# and checks its SHA-256. The SRPM chroot is minimal, so install cmake when
	# absent, the same way git is above.
	command -v cmake >/dev/null || dnf -y install cmake
	cmake -DPHOSPHOR_DEPS_FETCH=/tmp/$(NAME)-srpm -P cmake/PlasmaZonesDependencies.cmake

	# Build SRPM
	mkdir -p /tmp/$(NAME)-rpmbuild
	rpmbuild -bs $(SPEC) \
		--define "_sourcedir /tmp/$(NAME)-srpm" \
		--define "_srcrpmdir $(outdir)" \
		--define "_topdir /tmp/$(NAME)-rpmbuild"

	@echo "=== SRPM written to $(outdir) ==="

.PHONY: srpm
