import os
from io import StringIO, BytesIO
import tempfile
import datetime

import pytest

from voip_rec_email import (
    read_chunks, write_mixed, chain, tempfile_ctx, user_email, AudioFormat,
    RecordingType, get_config, noop_ctx)


def test_tempfile_ctx():
    with tempfile_ctx() as fp:
        assert os.path.isfile(fp)
        with open(fp, 'w') as f:
            f.write('foo')
    assert not os.path.isfile(fp)


def test_noop_ctx():
    with noop_ctx() as x:
        assert x is None


def test_read_chunks():
    def mk_fh():
        # StringIO just to save writing `b` everywhere!
        return StringIO('Here is a string')
    assert list(read_chunks(mk_fh())) == ['Here is a string']
    assert list(read_chunks(mk_fh(), 2)) == [
        'He', 're', ' i', 's ', 'a ', 'st', 'ri', 'ng']
    fh = mk_fh()
    fh.read(1)
    assert list(read_chunks(fh, 2)) == [
        'er', 'e ', 'is', ' a', ' s', 'tr', 'in', 'g']


def test_write_mixed():
    out = BytesIO()
    in1 = BytesIO(b'hello')
    in2 = BytesIO(b'greetings')
    write_mixed(out, [in1, 'world', in2, b'planet'])
    assert out.getvalue() == b'helloworldgreetingsplanet'


@pytest.mark.parametrize(
    'to_emails', [('admin@b.com',), ('burt@b.com', 'ernie@b.com')])
@pytest.mark.parametrize(
    'separator', [',', ';'])
def test_get_config(to_emails, separator):
    email_field = separator.join(to_emails)
    mail_date = datetime.datetime(
        2018, 5, 29, 10, 30, 12,
        tzinfo=datetime.timezone(datetime.timedelta(0, 3600)))
    minimal_env = {
        'duration': 'sometime',
        'from': 'alice@a.com',
        'to': 'bob@b.com',
        'email': email_field,
        'i': '<call-id>',
        'wavpath': '/tmp/some.wav',
        'maildate': 'Tue, 29 May 2018 10:30:12 +0100'
    }
    minimal_expected_config = {
        'duration': 'sometime',
        'from': 'alice@a.com',
        'to': 'bob@b.com',
        'i': '<call-id>',
        'maildate': mail_date,
    }
    assert get_config(minimal_env) == dict({
        'format': AudioFormat.wav,
        'type': RecordingType.recording,
        'trim': False,
        'normalise': False,
        'recipient_details': [('Recording', a) for a in to_emails],
        'wavpath': '/tmp/some.wav'}, **minimal_expected_config)
    full_env = dict(
        minimal_env,
        name='Altname',
        email=email_field + '/MTVN'
    )
    assert get_config(full_env) == dict({
        'recipient_details':  [('Altname', a) for a in to_emails],
        'format': AudioFormat.mp3,
        'type': RecordingType.voicemail,
        'trim': True,
        'normalise': True,
        'wavpath': '/tmp/some.wav'}, **minimal_expected_config)
    # Check invalid flag rejected:
    pytest.raises(ValueError, get_config, dict(
        minimal_env,
        email='admin@b.com/J'))
    # Check missing wavpath errors:
    pytest.raises(Exception, get_config, dict(minimal_env, wavpath=None))


def test_chain():
    with tempfile.TemporaryFile() as f:
        f.write(b'world\nhello')
        f.seek(0)
        out = chain(chain(chain(f, ['cat']), ['sort']), ['head', '-n', '1'])
        assert out.read() == b'hello\n'


@pytest.mark.parametrize('dtmf', [(b'', b''), (b'1234', b'<xml>1234</xml>')])
def test_user_email(dtmf):
    '''This is really just to check that the function executes, rather than
    anything too specific'''
    dtmf_header_content, dtmf_xml = dtmf
    audio = BytesIO(b'lalalala')
    dtmf_xml = BytesIO(dtmf_xml) if dtmf_xml else None
    out = BytesIO()
    write_mixed(out, user_email(
        recording_type=RecordingType.voicemail,
        recording_duration='36:21',
        call_originator='alice@example.com',
        call_recipient='bob@example.com',
        recipient_details=[
            ('Charlie Parker', 'charlie@example.com'),
            ('Edith Smith', 'edith@example.com'),
        ],
        send_date=datetime.datetime(2018, 6, 4, 10, 12, 50),
        audio_format=AudioFormat.mp3,
        call_id='8765-4321',
        dtmf_header_content=dtmf_header_content,
        recording_fh=audio,
        dtmf_xml_fh=dtmf_xml,
    ))
    out = out.getvalue()
    assert RecordingType.voicemail.name.title().encode('utf-8') in out
    assert b'36:21' in out
    assert b'alice@example.com' in out
    assert b'bob@example.com' in out
    assert b'Charlie Parker' in out
    assert b'charlie@example.com' in out
    assert b'Edith Smith' in out
    assert b'edith@example.com' in out
    assert b'Mon, 04 Jun 2018 10:12:50' in out
    assert b'2018-06-04 10:12:50' in out
    assert b'mp3' in out
    assert b'8765-4321.mp3' in out
    assert b'lalalala' in out
    if dtmf_header_content:
        assert b'XML attached' in out
        assert b'8765-4321.xml' in out
    else:
        assert b'XML attached' not in out
        assert b'8765-4321.xml' not in out
