hw_tee_modules = {}

if config_all_devices.has_key('CONFIG_VIRTIO_TEE')
  virtio_tee_ss = ss.source_set()
  virtio_tee_ss.add(when: 'CONFIG_VIRTIO_TEE',
                    if_true: [files('virtio-tee.c', 'virtio-tee-client.c', 'dlm.c' )])
  hw_tee_modules += {'virtio-tee': virtio_tee_ss}
endif

if config_all_devices.has_key('CONFIG_VIRTIO_PCI')
  virtio_tee_pci_ss = ss.source_set()
  virtio_tee_pci_ss.add(when: ['CONFIG_VIRTIO_TEE', 'CONFIG_VIRTIO_PCI'],
                        if_true: [files('virtio-tee-pci.c')])
  hw_tee_modules += {'virtio-tee-pci': virtio_tee_pci_ss}
endif

modules += { 'hw-tee': hw_tee_modules }
