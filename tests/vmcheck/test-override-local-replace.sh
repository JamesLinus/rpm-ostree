#!/bin/bash
#
# Copyright (C) 2017 Red Hat, Inc.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the
# Free Software Foundation, Inc., 59 Temple Place - Suite 330,
# Boston, MA 02111-1307, USA.

set -e

. ${commondir}/libtest.sh
. ${commondir}/libvm.sh

set -x

REPO=/tmp/vmcheck/yumrepo/packages/x86_64

# create a new vmcheck commit which has foo and bar in it already

# make sure the packages are not already layered
vm_assert_layered_pkg foo absent
vm_assert_layered_pkg bar absent
vm_assert_status_jq \
  '.deployments[0]["base-checksum"]|not' \
  '.deployments[0]["pending-base-checksum"]|not' \
  '.deployments[0]["base-local-replacements"]|length == 0' \
  '.deployments[0]["requested-base-local-replacements"]|length == 0'

vm_cmd ostree refs $(vm_get_booted_csum) --create vmcheck_tmp/without_foo_and_bar

# create a new branch with foo
vm_build_rpm foo
vm_build_rpm bar
vm_build_rpm fooext requires "foo = 1.0-1"
vm_rpmostree install fooext bar
vm_cmd ostree refs $(vm_get_deployment_info 0 checksum) \
  --create vmcheck_tmp/with_foo_and_bar
vm_rpmostree cleanup -p

# upgrade to new commit with foo in the base layer
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_bar
vm_rpmostree upgrade
vm_reboot
if ! vm_has_packages foo bar fooext; then
    assert_not_reached "foo, fooext, or bar not in base layer"
fi
echo "ok setup"

assert_replaced_local_pkg() {
  old_nevra=$1; shift
  new_nevra=$1; shift

  # funky jq syntax here: base-local-replacements is a list of (new, old)
  # tuples. The first [] is to print out all the tuples, the following [0/1] is
  # to access the new/old element of the tuple, and the [0] is to access the
  # nevra. We wrap the whole thing in [...] so that we get a list of nevra
  # strings to which we can then apply the index filter.

  vm_assert_status_jq \
    "[.deployments[0][\"base-local-replacements\"][][0][0]]|index(\"$new_nevra\") >= 0" \
    "[.deployments[0][\"base-local-replacements\"][][1][0]]|index(\"$old_nevra\") >= 0" \
    ".deployments[0][\"requested-base-local-replacements\"]|index(\"$new_nevra\") >= 0"
  root=$(vm_get_deployment_root 0)
  if ! vm_cmd rpm --dbpath $root/usr/share/rpm -q $new_nevra; then
    assert_not_reached "new pkg not in rpmdb?"
  fi
  name=${new_nevra%%-*}
  if ! vm_cmd $root/usr/bin/$name | grep -q $new_nevra; then
    assert_not_reached "new pkg not in tree?"
  fi
}

# try to replace foo without replacing the extension
vm_build_rpm foo version 2.0
if vm_rpmostree ex override replace $REPO/foo-2.0-1.x86_64.rpm 2>err.txt; then
  assert_not_reached "successfully replaced foo without fooext?"
fi
assert_file_has_content err.txt "fooext"
echo "ok failed to replace foo without fooext"

vm_build_rpm fooext version 2.0 requires "foo = 2.0-1"
vm_rpmostree ex override replace $REPO/foo{,ext}-2.0-1.x86_64.rpm
vm_assert_status_jq \
  '.deployments[0]["base-local-replacements"]|length == 2' \
  '.deployments[0]["requested-base-local-replacements"]|length == 2'
assert_replaced_local_pkg foo-1.0-1.x86_64 foo-2.0-1.x86_64
assert_replaced_local_pkg fooext-1.0-1.x86_64 fooext-2.0-1.x86_64
echo "ok override replace foo and fooext"

# replace bar with older version
vm_build_rpm bar version 0.9
vm_rpmostree ex override replace $REPO/bar-0.9-1.x86_64.rpm
vm_assert_status_jq \
  '.deployments[0]["base-local-replacements"]|length == 3' \
  '.deployments[0]["requested-base-local-replacements"]|length == 3'
assert_replaced_local_pkg foo-1.0-1.x86_64 foo-2.0-1.x86_64
assert_replaced_local_pkg fooext-1.0-1.x86_64 fooext-2.0-1.x86_64
assert_replaced_local_pkg bar-1.0-1.x86_64 bar-0.9-1.x86_64
echo "ok override replace bar"

vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck
vm_rpmostree upgrade
vm_assert_status_jq \
  '.deployments[0]["base-local-replacements"]|length == 3' \
  '.deployments[0]["requested-base-local-replacements"]|length == 3'
assert_replaced_local_pkg foo-1.0-1.x86_64 foo-2.0-1.x86_64
assert_replaced_local_pkg fooext-1.0-1.x86_64 fooext-2.0-1.x86_64
assert_replaced_local_pkg bar-1.0-1.x86_64 bar-0.9-1.x86_64
echo "ok override replacements carried through upgrade"

# try to reset pkgs using both name and nevra
vm_rpmostree ex override reset foo fooext-2.0-1.x86_64
vm_assert_status_jq \
  '.deployments[0]["base-local-replacements"]|length == 1' \
  '.deployments[0]["requested-base-local-replacements"]|length == 1'
assert_replaced_local_pkg bar-1.0-1.x86_64 bar-0.9-1.x86_64
echo "ok override reset foo and fooext using name and nevra"

vm_rpmostree ex override reset --all
vm_assert_status_jq \
  '.deployments[0]["base-removals"]|length == 0' \
  '.deployments[0]["requested-base-removals"]|length == 0'
echo "ok override reset --all"

vm_rpmostree cleanup -p

# test inactive replacements
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_bar
vm_rpmostree upgrade
vm_rpmostree ex override replace $REPO/bar-0.9-1.x86_64.rpm
vm_assert_status_jq \
  '.deployments[0]["base-local-replacements"]|length == 1' \
  '.deployments[0]["requested-base-local-replacements"]|length == 1'
assert_replaced_local_pkg bar-1.0-1.x86_64 bar-0.9-1.x86_64
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/without_foo_and_bar
vm_rpmostree upgrade
vm_assert_status_jq \
  '.deployments[0]["base-local-replacements"]|length == 0' \
  '.deployments[0]["requested-base-local-replacements"]|length == 1' \
  '.deployments[0]["requested-base-local-replacements"]|index("bar-0.9-1.x86_64") >= 0'
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_bar
vm_rpmostree upgrade
vm_assert_status_jq \
  '.deployments[0]["base-local-replacements"]|length == 1' \
  '.deployments[0]["requested-base-local-replacements"]|length == 1'
assert_replaced_local_pkg bar-1.0-1.x86_64 bar-0.9-1.x86_64
echo "ok active -> inactive -> active override replace"

vm_rpmostree cleanup -p

# try both local package layering and local replacements to
# make sure fd sending doesn't get mixed up
vm_build_rpm baz
vm_cmd ostree commit -b vmcheck --tree=ref=vmcheck_tmp/with_foo_and_bar
vm_rpmostree upgrade
vm_rpmostree ex override replace $REPO/bar-0.9-1.x86_64.rpm \
                       --install $REPO/baz-1.0-1.x86_64.rpm
vm_assert_status_jq \
  '.deployments[0]["base-local-replacements"]|length == 1' \
  '.deployments[0]["requested-base-local-replacements"]|length == 1' \
  '.deployments[0]["requested-local-packages"]|length == 1' \
  '.deployments[0]["requested-local-packages"]|index("baz-1.0-1.x86_64") >= 0'
assert_replaced_local_pkg bar-1.0-1.x86_64 bar-0.9-1.x86_64
echo "ok local replace and local layering"
