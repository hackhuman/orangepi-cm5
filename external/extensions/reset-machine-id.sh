# reset-machine-id
#
# The build bakes a single /etc/machine-id into the rootfs cache (and from it,
# debootstrap/dbus installs it into every image). When that image is flashed to
# many boards, each board boots with the SAME machine-id. systemd derives a
# "persistent" ethernet MAC from the machine-id ("net.ifnames + MACAddressPolicy
# = persistent"), so all boards also end up with the SAME generated MAC --
# which is exactly the symptom of "reflashing the same image always gives the
# same MAC".
#
# Fix: blank /etc/machine-id in the just-finished image, right before it is
# unmounted. On first boot systemd sees an empty machine-id and generates a
# unique one per board, so every board gets its own stable MAC (and host-id).
#
# This is the same approach upstream Armbian uses.

function pre_umount_final_image__reset_machine_id() {
	display_alert "Resetting machine-id" "unique per first boot" "info"

	# Truncating (not deleting) is deliberate: some tooling expects the file to
	# exist; an empty file is enough for systemd-machine-id-setup to re-seed it.
	truncate -s 0 "${MOUNT}/etc/machine-id"

	# The D-Bus copy is usually a symlink to /etc/machine-id; drop it so it does
	# not shadow a fresh machine-id with a stale one.
	rm -f "${MOUNT}/var/lib/dbus/machine-id"
}