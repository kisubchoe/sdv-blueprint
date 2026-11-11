#!/usr/bin/env bash
set -euo pipefail
# Multi-node orchestrator: stop/teardown flow
# Env overrides: see config.env

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=./config.env
source "${SCRIPT_DIR}/config.env"
# shellcheck source=./lib/utils.sh
source "${SCRIPT_DIR}/lib/utils.sh"

main() {
  require_cmd ssh
  require_cmd timeout
  resolve_auth_mode
  info "Auth mode: USE_SSHPASS=$USE_SSHPASS"

  [[ -n "${MASTER_NODE_IP}" ]] || { err "MASTER_NODE_IP is not set. Please set it in config.env."; exit 1; }
  [[ -n "${GUEST_NODE_IP}" ]]  || { err "GUEST_NODE_IP is not set. Please set it in config.env."; exit 1; }

  info "Step 1) master: docker compose down"
  on_master "cd '$MASTER_SERIAL_BRIDGE_DIR'; docker compose down"

  info "Step 2) guest: monitoring stop.sh"
  on_guest "cd '$GUEST_MONITORING_DIR'; ./stop.sh"

  info "Step 3) stop timpani (guest: timpani-n package, master: timpani-o container)"
  on_guest_sudo  "bash '$GUEST_TEST_SCRIPT_DIR/timpani/scripts/uninstall-timpani-n.sh'"
  on_master_sudo "bash '$MASTER_TEST_SCRIPT_DIR/timpani/scripts/uninstall-timpani-o.sh'"

  info "Removing guest-to-TIMPANI-O forwarding rule"
  # Use the same node addresses as run; stop before changing config.env.
  local timpani_forward_rule
  timpani_forward_rule="-s '${GUEST_NODE_IP}/32' -p tcp --dport 7777 -m conntrack --ctstate NEW,ESTABLISHED \
    --ctorigdst '${MASTER_NODE_IP}' --ctorigdstport 7777 -m conntrack --ctstate DNAT -m comment --comment timpani-managed -j ACCEPT"

  # Also remove the legacy rule that had no ACCEPT target.
  local rule
  for rule in "${timpani_forward_rule}" "${timpani_forward_rule% -j ACCEPT}"; do
    on_master_sudo "
      if check_output=\$(LC_ALL=C iptables -w 5 -C FORWARD ${rule} 2>&1); then
        iptables -w 5 -D FORWARD ${rule}
      else
        rc=\$?
        if [ \"\$rc\" -ne 1 ] || [[ -n \"\$check_output\" && \"\$check_output\" != 'iptables: Bad rule (does a matching rule exist in that chain?).' ]]; then
          printf '%s\n' \"\$check_output\" >&2
          echo '[ERROR] Failed to check TIMPANI forwarding rule' >&2
          exit \"\$rc\"
        fi
        echo '[INFO] TIMPANI forwarding rule already absent'
      fi
    "
  done

  info "Step 4) master: pullpiri runtime uninstall script"
  on_master_sudo "bash '$MASTER_TEST_SCRIPT_DIR/pullpiri/scripts/uninstall-pullpiri.sh' '$GUEST_HOST' '$GUEST_PORT' '$GUEST_USER' '$GUEST_PASS' '$GUEST_SUDO_PASS' '$GUEST_TEST_SCRIPT_DIR'"

  info "Step 5) guest: podman rm -f --all"
  on_guest_sudo "podman rm -f --all || true"

  info "Teardown done."
}

main "$@"
