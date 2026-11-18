import { mount } from '@vue/test-utils'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { ref } from 'vue'

import AudioVideo from '../../src_assets/common/assets/web/configs/tabs/AudioVideo.vue'
import configTabs from '../../src_assets/common/assets/web/configs/config_tabs.json'
import configDocumentation from '../../docs/configuration.md?raw'
import configurationShortcuts from '../../docs/configuration.js?raw'

function mountAudioVideo(platform) {
  const config = structuredClone(configTabs.find(tab => tab.id === 'av').options)
  const wrapper = mount(AudioVideo, {
    props: { platform, config },
    global: {
      mocks: { $t: key => key },
      provide: {
        platform: ref(platform),
        i18n: { t: key => key },
      },
      stubs: {
        AdapterNameSelector: true,
        DisplayOutputSelector: true,
        DisplayDeviceOptions: true,
        DisplayModesSettings: true,
      },
    },
  })
  return { wrapper, config }
}

afterEach(() => {
  vi.restoreAllMocks()
  document.body.innerHTML = ''
})

describe('externally managed audio configuration', () => {
  it('renders after the existing Windows audio controls and preserves its saved value', async () => {
    const { wrapper, config } = mountAudioVideo('windows')
    expect(wrapper.findAll('input').map(input => input.attributes('id'))).toEqual([
      'audio_sink',
      'virtual_sink',
      'install_steam_audio_drivers',
      'stream_audio',
      'external_audio',
      'mic_passthrough',
      'mic_device',
      'mic_set_default',
    ])
    expect(config.external_audio).toBe('disabled')
    expect(wrapper.get('#external_audio').element.checked).toBe(false)
    await wrapper.get('#external_audio').setValue(true)
    expect(config.external_audio).toBe('enabled')
    await wrapper.get('#external_audio').setValue(false)
    expect(config.external_audio).toBe('disabled')
    wrapper.unmount()
  })

  it.each(['linux', 'freebsd', 'macos'])('keeps the Windows-only option hidden on %s', platform => {
    const { wrapper } = mountAudioVideo(platform)
    expect(wrapper.find('#external_audio').exists()).toBe(false)
    expect(wrapper.find('#stream_audio').exists()).toBe(true)
    wrapper.unmount()
  })

  it('shows microphone passthrough options on Windows and Linux only', () => {
    const windows = mountAudioVideo('windows')
    expect(windows.config.mic_passthrough).toBe('enabled')
    expect(windows.config.mic_set_default).toBe('enabled')
    expect(windows.config.mic_device).toBe('')
    expect(windows.wrapper.get('#mic_passthrough').element.checked).toBe(true)
    windows.wrapper.unmount()

    const linux = mountAudioVideo('linux')
    expect(linux.wrapper.find('#mic_passthrough').exists()).toBe(true)
    expect(linux.wrapper.find('#mic_set_default').exists()).toBe(true)
    expect(linux.wrapper.find('#mic_device').exists()).toBe(false)
    linux.wrapper.unmount()

    const macos = mountAudioVideo('macos')
    expect(macos.wrapper.find('#mic_passthrough').exists()).toBe(false)
    macos.wrapper.unmount()
  })

  it('provides the standard documentation table and a working configuration shortcut', () => {
    const section = configDocumentation.split('### external_audio\n')[1].split('\n### ')[0]
    // Doxygen emits configuration headings as h2 and gives tables this class.
    document.body.innerHTML = '<input id="host-authority" value="localhost:47990">'
      + '<h2>external_audio</h2>' + section.replace('<table>', '<table class="doxtable">')
    const rows = [...document.querySelectorAll('tr')]
    expect(rows.map(row => row.cells[0].textContent.trim())).toEqual(['Description', 'Default', 'Example'])
    expect(rows[0].textContent).toContain('This option is only supported on Windows.')
    expect(rows[1].textContent).toContain('disabled')
    expect(rows[2].textContent).toContain('external_audio = enabled')

    new Function('document', configurationShortcuts)(document)
    document.dispatchEvent(new Event('DOMContentLoaded'))
    const button = document.querySelector('.open-button')
    expect(button.textContent).toBe('Open configuration')
    const open = vi.spyOn(window, 'open').mockImplementation(() => null)
    button.click()
    expect(open).toHaveBeenCalledWith('https://localhost:47990/config/#external_audio', '_blank', 'noopener')
  })
})
