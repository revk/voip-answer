from setuptools import setup, find_packages

setup(
    name='voip-rec-email',
    packages=find_packages('voip-rec-email'),
    package_dir={'': 'voip-rec-email'},
    decsription='Script for emailing voicemail recordings to customers',
    keywords=['voip', 'recording'],
    url='',
    author='',
    author_email='',
    version='',
    license='',
    install_requires=[
    ],
    extras_require={
        'test': ['flake8', 'pytest', 'pytest-cov']
    }
)
