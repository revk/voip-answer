#!/usr/bin/env python3

import os
import sys
import logging
import re
from logging.handlers import SysLogHandler
from enum import Enum
import subprocess
from tempfile import mkstemp
from contextlib import contextmanager
from datetime import datetime


log = logging.getLogger('voip-rec-email')


@contextmanager
def tempfile_ctx():
    (fileno, file_path) = mkstemp()
    try:
        os.close(fileno)
        yield file_path
    finally:
        os.unlink(file_path)


def read_chunks(fh, chunk_size=128 * 1024):
    while True:
        chunk = fh.read(chunk_size)
        if chunk:
            yield chunk
        else:
            return


def write_mixed(fh, items):
    '''Takes a mixed collection of strings or file handles and writes
    their contents in order to the given file handle.
    '''
    for i in items:
        if hasattr(i, 'read'):
            for chunk in read_chunks(i):
                fh.write(chunk)
        elif isinstance(i, str):
            fh.write(i.encode('utf-8'))
        else:
            # Presume bytestring
            fh.write(i)


RecordingType = Enum('RecordingType', ('recording', 'voicemail'))
AudioFormat = Enum('AudioFormat', ('wav', 'mp3', 'ogg', 'flac'))
input_date_format = '%a, %d %b %Y %H:%M:%S %z'
# FIXME: timezone?
rfc_3339_date_format = '%Y-%m-%d %H:%M:%S %z'
rfc_822_date_format = '%a, %d %b %Y %H:%M:%S %z'
email_fmt = '"{name}" <{address}>'


def extract_emails(email_addr_string):
    # Users have sometimes specified multiple email addresses in their email
    # address field. We should handle that by splitting if appropriate and
    # always handing back a sequence of email addresses.
    return (addr.strip() for addr in re.split(';|,', email_addr_string))


def extract_flags(email_with_flags):
    rec_type = RecordingType.recording
    trim = normalise = False
    encoding = AudioFormat.wav
    if '/' in email_with_flags:
        email_flags = email_with_flags.split('/', 1)
        email = email_flags[0]
        for flag in email_flags[1]:
            if flag == 'M':
                encoding = AudioFormat.mp3
            elif flag == 'O':
                encoding = AudioFormat.ogg
            elif flag == 'F':
                encoding = AudioFormat.flac
            elif flag == 'T':
                trim = True
            elif flag == 'V':
                rec_type = RecordingType.voicemail
            elif flag == 'N':
                normalise = True
            else:
                raise ValueError("Unrecognised flag: " + flag)
    else:
        email = email_with_flags
    return {
        'email': email,
        'format': encoding,
        'type': rec_type,
        'trim': trim,
        'normalise': normalise}


def get_config(env_dict):
    # These environment variables are set for us by the voip-answer program:
    var_names = [
        'maildate',  # of the form YYYY-MM-DD HH:MM:SS
        'duration',  # the duration of the call WHAT FORMAT?
        'from',  # call originator
        'to',  # call recipient
        'email',  # address this script should email
        'name',  # Possibly the SIP display name???
        'i',  # The truncated SIP call ID
        'wavpath',  # Path to the temporary file containing the call recording
    ]
    env_vars = {ev: env_dict.get(ev) for ev in var_names}
    # Normalise some of the environment:
    env_vars['maildate'] = datetime.strptime(
        env_vars['maildate'], input_date_format)
    env_vars.update(extract_flags(env_dict['email']))
    name = env_vars.pop('name') or 'Recording'
    env_vars['recipient_details'] = [
        (name, addr) for addr in extract_emails(env_vars.pop('email'))]
    env_vars['wavpath'] = os.path.normpath(env_vars['wavpath'])
    return env_vars


def chain(fh, *args, **kwargs):
    '''Spawn a subprocess that reads from the given stdin file handle and writes
    to a new file handle that it returns.
    '''
    return subprocess.Popen(
        *args, stdin=fh, stdout=subprocess.PIPE, **kwargs).stdout


def process_wav(fh):
    return fh


def process_mp3(fh):
    fh1 = chain(fh, [
        'sox', '-t', 'wav', '-', '-t', 'wav', '-e', 'signed-integer',
        '-r44.1k', '-'])
    return chain(fh1, [
        'nice', '-19', 'lame', '-q', '9', '--preset', '44.1', '-', '-'])


def process_ogg(fh):
    return chain(fh, ['sox', '-t', 'wav', '-', '-t', 'vorbis', '-'])


def process_flac(fh):
    return chain(fh, ['sox', '-t', 'wav', '-', '-t', 'flac', '-'])


audio_processors = {
    AudioFormat.wav: process_wav,
    AudioFormat.mp3: process_mp3,
    AudioFormat.ogg: process_ogg,
    AudioFormat.flac: process_flac
}


def process_audio(fh0, audio_format):
    fh1 = audio_processors[audio_format](fh0)
    # NB: we spawn a subprocess to base64 encode the audio in a streaming
    # fashion because it appears that all the current native Python base64
    # encoding libraries need to work on the whole file in memory!
    return chain(fh1, ['base64'])


def format_recipient_details(recipient_details):
    return ','.join(
        email_fmt.format(name=n, address=a) for n, a in recipient_details)


def user_email(
        recording_type, recording_duration, call_originator, call_recipient,
        recipient_details, send_date, audio_format, call_id,
        dtmf_header_content, recording_fh, dtmf_xml_fh):
    yield '''From: "{recording_type}" <{call_id}@recordings.aa.net.uk>
To: {recipient_details}
Date: {machine_send_date}
Subject: {recording_type} {duration} {call_originator} {call_recipient}
MIME-Version: 1.0
Content-Type: multipart/mixed; boundary="CUT-HERE"
'''.format(
        call_id=call_id,
        recording_type=recording_type.name.title(),
        duration=recording_duration,
        recipient_details=format_recipient_details(recipient_details),
        machine_send_date=send_date.strftime(rfc_822_date_format),
        call_originator=call_originator,
        call_recipient=call_recipient,
    )
    if dtmf_header_content:
        yield 'X-DTMF: ' + dtmf_header_content.decode('utf-8')
    yield '''
This is a multi-part message in MIME format.
--CUT-HERE
Content-Type: text/plain; charset=UTF-8

Attached is the call recording/voicemail that we have made on your
behalf as requested.

We do not keep a copy of this recording for you.
Do remember that the other party to the call may also have recorded it.

It is your responsibility to ensure that the call recording was made
legally and ensure that it is only disclosed to parties as permitted by
relevant laws.

Date:\t{human_send_date}
Length:\t{recording_duration}
From:\t{call_originator}
To:\t{call_recipient}
'''.format(
        human_send_date=send_date.strftime(rfc_3339_date_format),
        recording_duration=recording_duration,
        call_originator=call_originator,
        call_recipient=call_recipient,
    )
    if dtmf_header_content:
        yield 'DTMF: {} (XML attached)'.format(
            dtmf_header_content.decode('utf-8'))
    yield '''
--CUT-HERE
Content-Type: audio/{audio_format.name}; name={call_id}.{audio_format.name}
Content-Transfer-Encoding: base64
Content-Disposition: attachment;filename={call_id}.{audio_format.name}

'''.format(call_id=call_id, audio_format=audio_format)
    yield recording_fh
    if dtmf_xml_fh:
        yield '''
--CUT-HERE
Content-Type: application/xml; name={call_id}.xml
Content-Disposition: attachment;filename={call_id}.xml

'''.format(call_id=call_id)
        yield dtmf_xml_fh
        yield '\n'
    yield '--CUT-HERE--\n'


@contextmanager
def noop_ctx(value=None):
    yield value


if __name__ == '__main__':
    log.setLevel(logging.INFO)
    log.addHandler(SysLogHandler(address='/dev/log'))
    try:
        config = get_config(os.environ)
    except Exception:
        log.exception('Unable to determine desired behaviour')
        raise
    log.info(
        'Preparing recording email for: %s',
        format_recipient_details(config['recipient_details']))
    try:
        with tempfile_ctx() as temp_path:
            dtmf_header_content = subprocess.check_output([
                'dtmf2xml', '--text', '--infile={}'.format(config['wavpath']),
                '--outfile={}'.format(temp_path)]
            )
            with open(config['wavpath'], 'rb') as wav:
                base64_audio_fh = process_audio(wav, config['format'])
                sendmail = subprocess.Popen([
                    '/usr/sbin/sendmail',
                    '-f', 'noreply@recordings.aa.net.uk',
                    '-i', '-t'], stdin=subprocess.PIPE)
                if dtmf_header_content:
                    ctx = open(temp_path, 'rb')
                else:
                    ctx = noop_ctx()
                with ctx as dtmf_xml_fh:
                    write_mixed(sendmail.stdin, user_email(
                        recording_type=config['type'],
                        recording_duration=config['duration'],
                        call_originator=config['from'],
                        call_recipient=config['to'],
                        recipient_details=config['recipient_details'],
                        send_date=config['maildate'],
                        audio_format=config['format'],
                        call_id=config['i'],
                        dtmf_header_content=dtmf_header_content,
                        recording_fh=base64_audio_fh,
                        dtmf_xml_fh=dtmf_xml_fh,
                        ))
                sendmail.stdin.close()
                sendmail_exit = sendmail.wait()
    except BaseException:
        log.exception(
            'Failed to send recording email to: ' +
            ','.join(config['recipient_details'][0]))
        raise
    else:
        if config['wavpath']:
            os.unlink(config['wavpath'])
        log.info(
            'Sent recording email to: ' +
            ','.join(config['recipient_details'][0]))
        if sendmail_exit:
            sys.exit(sendmail_exit)
