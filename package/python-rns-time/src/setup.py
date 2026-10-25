from setuptools import setup

setup(
    name="rns-time",
    version="0.2.0",
    description="Authenticated time over Reticulum (NTP-style)",
    long_description=open("README.md", encoding="utf-8").read(),
    long_description_content_type="text/markdown",
    packages=["rns_time"],
    python_requires=">=3.7",
    install_requires=["rns"],
    entry_points={
        "console_scripts": [
            "rns-time-client=rns_time.client:main",
            "rns-time-server=rns_time.server:main",
        ],
    },
)
