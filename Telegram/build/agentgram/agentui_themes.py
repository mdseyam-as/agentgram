#!/usr/bin/env python3
"""Generate the AgentUI palettes (Resources/agentui-*.tdesktop-theme).

AgentUI is the frosted glass design of Agentgram. Each palette starts
from a full Telegram palette (lib_ui defaults plus a built-in theme),
moves the blue-grey neutrals to a calm graphite / porcelain scale and
the Telegram blue accents to the AgentUI lagoon accent, then applies
the explicit keys below. Surfaces that AgentUI paints as glass keep an
opaque color here: AgentUi::GlassPalette() makes them transparent only
inside the glass widgets, everywhere else they stay safe solid fills.

Run from anywhere: python3 Telegram/build/agentgram/agentui_themes.py
"""

import colorsys
import io
import re
import struct
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PALETTE = ROOT / 'lib_ui' / 'ui' / 'colors.palette'
RESOURCES = ROOT / 'Resources'

LINE = re.compile(r'^([A-Za-z][A-Za-z0-9_]*)\s*:\s*([^;]+);')


def parse(text):
    result = {}
    for line in text.splitlines():
        m = LINE.match(line.strip())
        if m:
            value = m.group(2).split('|')[0].strip()
            result[m.group(1)] = value
    return result


def read_theme(name):
    with zipfile.ZipFile(RESOURCES / f'{name}.tdesktop-theme') as z:
        return parse(z.read('colors.tdesktop-theme').decode('utf-8'))


def to_rgba(value):
    v = value.lstrip('#')
    r, g, b = (int(v[i:i + 2], 16) for i in (0, 2, 4))
    a = int(v[6:8], 16) if len(v) == 8 else 255
    return r, g, b, a


def to_hex(r, g, b, a=255):
    clamp = lambda x: max(0, min(255, int(round(x))))
    result = '#%02x%02x%02x' % (clamp(r), clamp(g), clamp(b))
    return result + ('%02x' % clamp(a) if clamp(a) != 255 else '')


def remap(value, scheme):
    """Moves Telegram blues to the AgentUI neutrals and accent."""
    if not value.startswith('#'):
        return value
    r, g, b, a = to_rgba(value)
    h, l, s = colorsys.rgb_to_hls(r / 255., g / 255., b / 255.)
    hue = h * 360.
    if not (185. <= hue <= 232.):
        return value
    if s > 0.42 and 0.25 < l < 0.85:
        # Accent family: keep the lightness relation of the original.
        nh = scheme['accent_hue'] / 360.
        ns = min(1., s * scheme['accent_sat'])
        nl = l * scheme['accent_light']
    else:
        # Neutral family: graphite (dark) or porcelain (light).
        nh = scheme['neutral_hue'] / 360.
        ns = s * scheme['neutral_sat']
        nl = l
    nr, ng, nb = colorsys.hls_to_rgb(nh, nl, ns)
    return to_hex(nr * 255, ng * 255, nb * 255, a)


ACCENT_DARK = '#5cc8d8'
ACCENT_LIGHT = '#0e8ea3'

DARK = {
    'base': 'night',
    'accent_hue': 189.,
    'accent_sat': 0.85,
    'accent_light': 1.02,
    'neutral_hue': 216.,
    'neutral_sat': 0.38,
    'keys': {
        # Ground and text.
        'windowBg': '#12151b',
        'windowFg': '#e9edf2',
        'windowBgOver': '#1b2028',
        'windowBgRipple': '#232a34',
        'windowFgOver': '#f2f5f8',
        'windowSubTextFg': '#8892a3',
        'windowSubTextFgOver': '#98a2b2',
        'windowBoldFg': '#f3f5f8',
        'windowBoldFgOver': '#ffffff',
        'windowBgActive': ACCENT_DARK,
        # White stays the text on dark overlays (service messages, media
        # controls); accent buttons get their own dark text below.
        'windowFgActive': '#ffffff',
        'windowActiveTextFg': '#79d6e3',
        'windowShadowFg': '#000000',
        'windowShadowFgFallback': '#0c0e12',
        'shadowFg': '#00000048',
        'activeButtonBg': ACCENT_DARK,
        'activeButtonBgOver': '#6fd2e0',
        'activeButtonBgRipple': '#8ddce8',
        'activeButtonFg': '#061a1e',
        'activeButtonFgOver': '#061a1e',
        'activeButtonSecondaryFg': '#0b3a42',
        'activeLineFg': ACCENT_DARK,
        'lightButtonBg': '#ffffff00',
        'lightButtonBgOver': '#ffffff0f',
        'lightButtonBgRipple': '#ffffff1a',
        'lightButtonFg': '#79d6e3',
        'scrollBarBg': '#ffffff38',
        'scrollBarBgOver': '#ffffff60',
        'scrollBg': '#ffffff0c',
        'scrollBgOver': '#ffffff18',
        'placeholderFg': '#6e7889',
        'placeholderFgActive': '#525b6a',
        'inputBorderFg': '#2a313c',
        'filterInputBorderFg': '#ffffff1f',
        'filterInputInactiveBg': '#ffffff0d',
        'filterInputActiveBg': '#ffffff14',
        'checkboxFg': '#5b6576',
        'sliderBgInactive': '#2c333f',
        'sliderBgActive': ACCENT_DARK,
        'tooltipBg': '#1d222b',
        'tooltipFg': '#e3e8ee',
        'tooltipBorderFg': '#2c333e',

        # Window title (Linux custom title bar).
        'titleBg': '#0d1014',
        'titleBgActive': '#0d1014',
        'titleShadow': '#00000000',
        'titleButtonBg': '#0d1014',
        'titleButtonBgActive': '#0d1014',
        'titleButtonBgOver': '#ffffff12',
        'titleButtonBgActiveOver': '#ffffff12',
        'titleButtonCloseBgActive': '#0d1014',
        'titleButtonCloseBg': '#0d1014',
        'titleFg': '#6c7585',
        'titleFgActive': '#a8b1bf',
        'titleButtonFg': '#6c7585',
        'titleButtonFgOver': '#e3e8ee',

        # Menus are frosted, the backdrop is painted below the tint.
        'menuBg': '#1a1f27',
        'menuBgOver': '#ffffff12',
        'menuBgRipple': '#ffffff1c',
        'menuIconFg': '#8a94a5',
        'menuIconFgOver': '#e9edf2',
        'menuSubmenuArrowFg': '#6c7585',
        'menuFgDisabled': '#4b5361',
        'menuSeparatorFg': '#ffffff14',

        # Boxes over the frosted layer.
        'layerBg': '#05070a6b',
        'boxBg': '#161a21',
        'boxSearchBg': '#161a21',
        'boxTitleFg': '#f3f5f8',
        'boxTextFgGood': '#79d6e3',
        'contactsBg': '#161a21',
        'contactsBgOver': '#1e242d',
        'contactsStatusFgOnline': '#79d6e3',
        'cancelIconFg': '#7a8495',
        'cancelIconFgOver': '#e3e8ee',

        # Chat list: glass, the active and hovered rows are drawn as
        # animated pills by the list itself.
        'dialogsBg': '#12151b',
        'dialogsBgOver': '#ffffff10',
        'dialogsBgActive': '#5cc8d829',
        'dialogsRippleBg': '#ffffff12',
        'dialogsRippleBgActive': '#5cc8d83d',
        'dialogsNameFg': '#eef1f5',
        'dialogsDateFg': '#7f899a',
        'dialogsTextFg': '#8892a3',
        'dialogsTextFgService': '#79d6e3',
        'dialogsSentIconFg': '#79d6e3',
        'dialogsSendingIconFg': '#6c7585',
        'dialogsUnreadBg': ACCENT_DARK,
        'dialogsUnreadBgMuted': '#3a414d',
        'dialogsUnreadFg': '#061a1e',
        'dialogsUnreadBgOver': ACCENT_DARK,
        'dialogsUnreadBgMutedOver': '#444c59',
        'dialogsOnlineBadgeFg': '#5fd6a0',
        'dialogsVerifiedIconBg': '#79d6e3',
        'dialogsVerifiedIconFg': '#0e1116',
        'dialogsNameFgActive': '#ffffff',
        'dialogsDateFgActive': '#a9dbe3',
        'dialogsTextFgActive': '#c8e6eb',
        'dialogsTextFgServiceActive': '#ffffff',
        'dialogsDraftFgActive': '#ffb4ae',
        'dialogsSendingIconFgActive': '#ffffff99',
        'dialogsSentIconFgActive': '#a9dbe3',
        'dialogsUnreadBgActive': '#ffffff',
        'dialogsUnreadBgMutedActive': '#ffffff73',
        'dialogsUnreadFgActive': '#0b2a30',
        'dialogsOnlineBadgeFgActive': '#ffffff',
        'dialogsVerifiedIconBgActive': '#ffffff',
        'dialogsVerifiedIconFgActive': '#0b2a30',
        'searchedBarBg': '#ffffff0a',
        'searchedBarFg': '#8892a3',

        # Folders side bar.
        'sideBarBg': '#0b0d11',
        'sideBarBgActive': '#ffffff0f',
        'sideBarBgRipple': '#ffffff14',
        'sideBarTextFg': '#7f899a',
        'sideBarTextFgActive': '#e9edf2',
        'sideBarIconFg': '#7f899a',
        'sideBarIconFgActive': '#79d6e3',
        'sideBarBadgeBg': ACCENT_DARK,
        'sideBarBadgeFg': '#061a1e',
        'sideBarBadgeBgMuted': '#4b5361',

        # Chat header and compose area are glass.
        'topBarBg': '#14181f',
        'historyComposeAreaBg': '#14181f',
        'historyComposeButtonBg': '#14181f',
        'historyComposeButtonBgOver': '#ffffff0d',
        'historyComposeButtonBgRipple': '#ffffff17',
        'historyComposeIconFg': '#8a94a5',
        'historyComposeIconFgOver': '#e9edf2',
        'historySendIconFg': ACCENT_DARK,
        'historySendIconFgOver': '#8ddce8',
        'historyReplyIconFg': ACCENT_DARK,
        'historyPinnedBg': '#14181f',
        'historyToDownBg': '#1c2129e0',
        'historyToDownBgOver': '#252b35e8',
        'historyToDownBgRipple': '#2e3540',
        'historyToDownFg': '#a8b1bf',
        'historyToDownShadow': '#00000059',
        'historyUnreadBarBg': '#ffffff0f',
        'historyUnreadBarFg': '#cfd6df',

        # Bubbles: frosted in / lagoon out.
        'msgInBg': '#1b2029e8',
        'msgInBgSelected': '#26404ae8',
        'msgOutBg': '#17434be8',
        'msgOutBgSelected': '#1f5560e8',
        'msgInShadow': '#00000000',
        'msgInShadowSelected': '#00000000',
        'msgOutShadow': '#00000000',
        'msgOutShadowSelected': '#00000000',
        'msgSelectOverlay': '#5cc8d82e',
        'msgStickerOverlay': '#5cc8d866',
        'historyTextOutFg': '#eef6f7',
        'historyLinkInFg': '#79d6e3',
        'historyLinkOutFg': '#9fe6f0',
        'historyOutIconFg': '#8ddce8',
        'historySendingOutIconFg': '#7fb8c1',
        'msgInDateFg': '#7f899a',
        'msgOutDateFg': '#8fb9c0',
        'msgInServiceFg': '#79d6e3',
        'msgOutServiceFg': '#a6e4ec',
        'msgInReplyBarColor': ACCENT_DARK,
        'msgOutReplyBarColor': '#9fe6f0',
        'msgServiceBg': '#0b0e1394',
        'msgServiceBgSelected': '#17434be0',
        'msgFileInBg': ACCENT_DARK,
        'msgFileInBgOver': '#6fd2e0',
        'msgFileOutBg': '#7fdbe6',
        'historyFileInIconFg': '#061a1e',
        'historyFileOutIconFg': '#061a1e',
        'historyFileInRadialFg': '#061a1e',
        'historyFileOutRadialFg': '#061a1e',
        'msgWaveformInActive': ACCENT_DARK,
        'msgWaveformInInactive': '#3a414d',
        'msgWaveformOutActive': '#a6e4ec',
        'msgWaveformOutInactive': '#2f6670',
        'msgDateImgBg': '#0b0e1380',
        'historyPeerUserpicFg': '#ffffff',
        'historyScrollBarBg': '#ffffff38',
        'historyScrollBarBgOver': '#ffffff60',
        'historyScrollBg': '#ffffff0c',
        'historyScrollBgOver': '#ffffff18',
        'emojiPanBg': '#161a21',
        'emojiPanCategories': '#12151b',
        'toastBg': '#1d222bf0',
        'toastFg': '#eef1f5',
        'importantTooltipFgLink': '#79d6e3',

        # Drawer, player, calls.
        'mainMenuBg': '#161a21',
        'mainMenuCoverBg': '#16303a',
        'mainMenuCloudBg': '#0f2a32',
        'mainMenuCloudFg': '#ffffff',
        'mediaPlayerActiveFg': ACCENT_DARK,
        'callBarBg': '#17434b',
        'callAnswerBg': '#3fbf8f',
        'notificationsBoxScreenBg': '#17434b',
        'introCoverTopBg': '#0f2a32',
        'introCoverBottomBg': '#17434b',
    },
}

LIGHT = {
    'base': 'day-blue',
    'accent_hue': 189.,
    'accent_sat': 0.9,
    'accent_light': 0.86,
    'neutral_hue': 212.,
    'neutral_sat': 0.45,
    'keys': {
        'windowBg': '#f6f7f9',
        'windowFg': '#11151b',
        'windowBgOver': '#eceff3',
        'windowBgRipple': '#e1e5eb',
        'windowSubTextFg': '#6b7586',
        'windowSubTextFgOver': '#606a7a',
        'windowBoldFg': '#0d1116',
        'windowBoldFgOver': '#0d1116',
        'windowBgActive': ACCENT_LIGHT,
        'windowFgActive': '#ffffff',
        'windowActiveTextFg': ACCENT_LIGHT,
        'shadowFg': '#0b1a2a1f',
        'activeButtonBg': ACCENT_LIGHT,
        'activeButtonBgOver': '#0b8092',
        'activeButtonBgRipple': '#087282',
        'activeButtonFg': '#ffffff',
        'activeButtonFgOver': '#ffffff',
        'activeButtonSecondaryFg': '#c4eef4',
        'activeLineFg': ACCENT_LIGHT,
        'lightButtonBg': '#ffffff00',
        'lightButtonBgOver': '#0e8ea312',
        'lightButtonBgRipple': '#0e8ea31f',
        'lightButtonFg': ACCENT_LIGHT,
        'scrollBarBg': '#0000003d',
        'scrollBarBgOver': '#00000066',
        'scrollBg': '#0000000d',
        'scrollBgOver': '#00000017',
        'filterInputBorderFg': '#0000001a',
        'filterInputInactiveBg': '#ffffff8c',
        'filterInputActiveBg': '#ffffffd9',
        'sliderBgActive': ACCENT_LIGHT,
        'tooltipBg': '#ffffff',
        'tooltipFg': '#11151b',
        'tooltipBorderFg': '#dfe3e8',

        'titleBg': '#e9edf1',
        'titleBgActive': '#e9edf1',
        'titleShadow': '#00000000',
        'titleButtonBg': '#e9edf1',
        'titleButtonBgActive': '#e9edf1',
        'titleButtonCloseBg': '#e9edf1',
        'titleButtonCloseBgActive': '#e9edf1',
        'titleButtonBgOver': '#0000000d',
        'titleButtonBgActiveOver': '#0000000d',
        'titleFg': '#8a93a1',
        'titleFgActive': '#4b5563',

        'menuBg': '#fbfcfd',
        'menuBgOver': '#0000000a',
        'menuBgRipple': '#00000012',
        'menuSeparatorFg': '#00000012',

        'layerBg': '#0f172a33',
        'boxBg': '#fbfcfd',
        'boxSearchBg': '#fbfcfd',
        'contactsBg': '#fbfcfd',
        'contactsBgOver': '#f0f3f6',
        'contactsStatusFgOnline': ACCENT_LIGHT,

        'dialogsBg': '#f6f7f9',
        'dialogsBgOver': '#0000000d',
        'dialogsBgActive': '#0e8ea31f',
        'dialogsRippleBg': '#00000012',
        'dialogsRippleBgActive': '#0e8ea32e',
        'dialogsNameFg': '#0d1116',
        'dialogsTextFg': '#606a7a',
        'dialogsDateFg': '#7a8392',
        'dialogsTextFgService': ACCENT_LIGHT,
        'dialogsSentIconFg': ACCENT_LIGHT,
        'dialogsUnreadBg': ACCENT_LIGHT,
        'dialogsUnreadBgOver': ACCENT_LIGHT,
        'dialogsUnreadBgMuted': '#b8c0cb',
        'dialogsUnreadBgMutedOver': '#aeb7c3',
        'dialogsOnlineBadgeFg': '#22b07d',
        'dialogsNameFgActive': '#06343c',
        'dialogsChatIconFgActive': '#06343c',
        'dialogsDateFgActive': '#2b6a75',
        'dialogsTextFgActive': '#1f5862',
        'dialogsTextFgServiceActive': '#06343c',
        'dialogsDraftFgActive': '#d5463c',
        'dialogsSendingIconFgActive': '#2b6a75',
        'dialogsSentIconFgActive': ACCENT_LIGHT,
        'dialogsUnreadBgActive': ACCENT_LIGHT,
        'dialogsUnreadBgMutedActive': '#9fb4bb',
        'dialogsUnreadFgActive': '#ffffff',
        'dialogsOnlineBadgeFgActive': '#22b07d',
        'dialogsVerifiedIconBgActive': ACCENT_LIGHT,
        'dialogsVerifiedIconFgActive': '#ffffff',
        'searchedBarBg': '#0000000a',

        'sideBarBg': '#eceff3',
        'sideBarBgActive': '#0000000d',
        'sideBarBgRipple': '#00000014',
        'sideBarTextFg': '#6b7586',
        'sideBarTextFgActive': '#0d1116',
        'sideBarIconFg': '#6b7586',
        'sideBarIconFgActive': ACCENT_LIGHT,
        'sideBarBadgeBg': ACCENT_LIGHT,
        'sideBarBadgeBgMuted': '#9aa3b0',

        'topBarBg': '#f8f9fb',
        'historyComposeAreaBg': '#f8f9fb',
        'historyComposeButtonBg': '#f8f9fb',
        'historyComposeButtonBgOver': '#0000000a',
        'historyComposeButtonBgRipple': '#00000012',
        'historySendIconFg': ACCENT_LIGHT,
        'historySendIconFgOver': '#0b8092',
        'historyReplyIconFg': ACCENT_LIGHT,
        'historyPinnedBg': '#f8f9fb',
        'historyToDownBg': '#ffffffe0',
        'historyToDownBgOver': '#f4f6f8f0',
        'historyToDownShadow': '#0b1a2a2e',
        'historyUnreadBarBg': '#ffffff8c',

        'msgInBg': '#ffffffdb',
        'msgInBgSelected': '#d9f1f5e6',
        'msgOutBg': '#d3f0f3e6',
        'msgOutBgSelected': '#bfe7ece6',
        'msgInShadow': '#00000000',
        'msgInShadowSelected': '#00000000',
        'msgOutShadow': '#00000000',
        'msgOutShadowSelected': '#00000000',
        'msgSelectOverlay': '#0e8ea326',
        'msgStickerOverlay': '#0e8ea34d',
        'historyTextOutFg': '#0d1116',
        'historyLinkInFg': ACCENT_LIGHT,
        'historyLinkOutFg': '#0a7686',
        'historyOutIconFg': ACCENT_LIGHT,
        'historySendingOutIconFg': '#6aa8b1',
        'msgOutDateFg': '#4f8a93',
        'msgInServiceFg': ACCENT_LIGHT,
        'msgOutServiceFg': '#0a7686',
        'msgInReplyBarColor': ACCENT_LIGHT,
        'msgOutReplyBarColor': '#0a7686',
        'msgServiceBg': '#33475b66',
        'msgFileInBg': ACCENT_LIGHT,
        'msgFileInBgOver': '#0b8092',
        'msgFileOutBg': ACCENT_LIGHT,
        'msgWaveformInActive': ACCENT_LIGHT,
        'msgWaveformOutActive': ACCENT_LIGHT,
        'msgWaveformOutInactive': '#9cd0d8',
        'emojiPanBg': '#fbfcfd',
        'toastBg': '#1b2029eb',

        'mainMenuBg': '#fbfcfd',
        'mainMenuCoverBg': ACCENT_LIGHT,
        'mainMenuCloudBg': '#0a6e7e',
        'mediaPlayerActiveFg': ACCENT_LIGHT,
        'callBarBg': ACCENT_LIGHT,
        'introCoverTopBg': '#0a6e7e',
        'introCoverBottomBg': ACCENT_LIGHT,
    },
}


def png(color):
    """A 1x1 png of the given color, the theme background placeholder.
    AgentUI sets its own gradient wallpaper right after the theme."""
    r, g, b, _ = to_rgba(color)
    raw = b'\x00' + bytes((r, g, b))

    def chunk(kind, data):
        body = kind + data
        return (struct.pack('>I', len(data)) + body
                + struct.pack('>I', zlib.crc32(body) & 0xffffffff))
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw))
            + chunk(b'IEND', b''))


def build(scheme, name, title):
    defaults = parse(PALETTE.read_text('utf-8'))
    base = read_theme(scheme['base'])
    merged = dict(defaults)
    merged.update((k, v) for k, v in base.items() if k in defaults)
    result = {}
    for key, value in merged.items():
        result[key] = remap(value, scheme)
    for key, value in scheme['keys'].items():
        if key not in defaults:
            print(f'  skipping unknown palette key: {key}')
            continue
        result[key] = value
    lines = [f'// {title}: generated by build/agentgram/agentui_themes.py']
    lines += [f'{key}: {value};' for key, value in result.items()]
    content = ('\n'.join(lines) + '\n').encode('utf-8')

    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        info = zipfile.ZipInfo('colors.tdesktop-theme', (2026, 10, 20, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(info, content)
        info = zipfile.ZipInfo('background.png', (2026, 10, 20, 0, 0, 0))
        z.writestr(info, png(result['windowBg']))
    path = RESOURCES / f'{name}.tdesktop-theme'
    path.write_bytes(out.getvalue())
    print(f'{path.relative_to(ROOT.parent)}: {len(result)} keys')


if __name__ == '__main__':
    build(DARK, 'agentui-dark', 'AgentUI Dark')
    build(LIGHT, 'agentui-light', 'AgentUI Light')
