#!/usr/bin/env python3
#
# Copyright 2022-2026 Morse Micro
#
# SPDX-License-Identifier: Apache-2.0
#

__doc__ = """
Module to help generate a single MM-IOT-SDK configure store partition.

This does NOT consider primary/staging partitions.
For more information about the configuration store see: src/mmconfig/mmconfig.c.

This can be used either as a standalone script or programmatically.
For device related features that require external modules, see program-configstore.py.
"""

import argparse
import json
import logging
import os
import re
import struct
import sys

# MMCS (Morse Micro Config Store) in little endian notation
MMCONFIG_SIGNATURE = 0x53434D4D

# Starting seed value for the Xorshift PRNG, aribtrarily chosen
XORHASH_SEED = 0xdfb7f3e1

# Flash erased value
ERASED_VALUE = 0xFF

MAX_KEY_LEN = 32

# Regular expresion used to validate config store keys
KEY_PATTERN = re.compile("[A-Za-z][A-Za-z0-9._]*")

# Number of times to attempt a read before giving up
MAX_READ_ATTEMPTS = 2


def _expand_filename_in_json(json_filename, filename):
    unexpanded_filename = os.path.expanduser(filename)
    filename = os.path.expandvars(unexpanded_filename)
    # Change file path relative to the JSON file, but only if it did not begin
    # with an environment variable. Note that if it begin with an env var then
    # the first character must have changed, since env vars start with $ or %.
    if unexpanded_filename[0] == filename[0] and not os.path.isabs(filename):
        if json_filename is None or json_filename == "-":
            logging.error(
                "File references in JSON without source filename must have absolute paths")
            sys.exit(1)
        filename = os.path.join(os.path.dirname(json_filename), filename)
    return filename


class XorHash:
    def __init__(self):
        self._checksum = XORHASH_SEED

    def update(self, data):
        for b in data:
            self._checksum = XorHash._xorhash_value(self._checksum + b)

    def update_u8(self, u8):
        self._checksum = XorHash._xorhash_value(self._checksum + u8)

    def update_u16(self, u16):
        self.update(struct.pack("<H", u16))

    def checksum(self):
        return self._checksum

    def _xorhash_value(val):
        # Implements a simple hashing algorithm based on the Xorshift PRNG
        # https://en.wikipedia.org/wiki/Xorshift (Marsaglia, 2003)
        val = (val ^ (val << 13)) & 0xFFFFFFFF
        val = (val ^ (val >> 17)) & 0xFFFFFFFF
        val = (val ^ (val << 5)) & 0xFFFFFFFF
        return val


class ConfigStoreDeserializeError(Exception):
    pass


class ConfigStorePartition:
    def __init__(self, version=0):
        self._dictionary = {}
        self._version = version

    def _serialize_key_value_pair(key, data):
        """
        Serialize the key/value pair in the following format:

        .. code-block:: text

            +------------+----------+--------------+-------------------+
            | Key Length | Key Name | Value Length | Value (raw bytes) |
            +------------+----------+--------------+-------------------+
                  1      <Key Length>       2          <Value Length>     (field size in octets)
        """
        result = struct.pack("<B", len(key)) + key.encode("ascii")
        result += struct.pack("<H", len(data)) + data
        return result

    def load_data(self, entries, json_filename=None, warn_on_error=False):
        """
        Load multiple items at once.

        Format:
        {
            "strings": { "<k>": "<v>" },
            "files": { "<k>": "<filename>" },
            "hex_strings": { "<k>": "<hexstring>" },
        }
        """
        for data_type, data in entries.items():
            if data_type == "strings":
                for k, v in data.items():
                    self.add_entry_string(k, v)
            elif data_type == "files":
                for k, v in data.items():
                    expanded_filename = _expand_filename_in_json(json_filename, v)
                    self.add_entry_file(k, expanded_filename)
            elif data_type == "hex_strings":
                for k, v in data.items():
                    self.add_entry_hex_string(k, v)
            elif warn_on_error:
                logging.warning(f"Data type '{data_type}' is unknown, skipping.")
            else:
                logging.error(f"Data type '{data_type}' is unknown, aborting.")
                sys.exit(1)

    def serialize(self, ostream):
        """
        Serialize the given ConfigStoreState data structure to the given file.
        """
        data = b""
        for k, v in self._dictionary.items():
            data += ConfigStorePartition._serialize_key_value_pair(k, v)

        # Compute checksum
        xorhash = XorHash()
        xorhash.update(data)

        ostream.write(struct.pack("<I", MMCONFIG_SIGNATURE))
        ostream.write(struct.pack("<I", self._version))
        ostream.write(struct.pack("<I", xorhash.checksum()))
        ostream.write(data)

        # Write a terminating 0xFF for Flashes that erase to non 0xFF values
        ostream.write((ERASED_VALUE).to_bytes(1, byteorder="little"))

    def deserialize(istream):
        verify_signature = istream.read_u32()
        version = istream.read_u32()
        checksum = istream.read_u32()

        xorhash = XorHash()

        partition = ConfigStorePartition(version=version)

        # Bail out if signature does not match
        if verify_signature != MMCONFIG_SIGNATURE:
            raise ConfigStoreDeserializeError("Invalid signature found")

        while not istream.is_at_eof():
            key_size = istream.read_u8()
            if key_size in [0, ERASED_VALUE]:
                # We reached the end of the list
                break

            # Sanity checks
            if key_size > MAX_KEY_LEN:
                raise ConfigStoreDeserializeError(
                    "Invalid key length in partition")

            xorhash.update_u8(key_size)

            raw_key = istream.read(key_size)
            xorhash.update(raw_key)

            value_size = istream.read_u16()
            if (value_size >> 8) == ERASED_VALUE:
                # Flash partially programmed, bomb out
                raise ConfigStoreDeserializeError("Corrupted partition detected")

            xorhash.update_u16(value_size)

            value = istream.read(value_size)
            xorhash.update(value)

            # Sanity checks
            try:
                key = raw_key.decode("ascii")
            except UnicodeDecodeError as e:
                raise ConfigStoreDeserializeError(f"Key contains non-ascii characters ({e})")

            if not KEY_PATTERN.fullmatch(key):
                raise ConfigStoreDeserializeError(f"Invalid characters found in key {key}")

            if len(key) > MAX_KEY_LEN:
                raise ConfigStoreDeserializeError(f"Key too long ({key})")

            partition._dictionary[key.lower()] = value
            logging.debug(f"Read {key}={value}")

        # Bail out if checksum does not match
        if xorhash.checksum() != checksum:
            raise ConfigStoreDeserializeError("Checksum failed")

        logging.debug("Successfully read partition with version %d", partition._version)
        return partition

    def copy_and_bump_version(self):
        partition = ConfigStorePartition(self._version + 1)
        partition._dictionary.update(self._dictionary)
        return partition

    def version(self):
        return self._version

    def _add_entry(self, type_name, data):
        if KEY_PATTERN.fullmatch(type_name) and len(type_name) <= MAX_KEY_LEN:
            # We do a .lower() as Python dictionaries are case sensitive but config store
            # is case insensitive and so would consider 'Key' and 'key' the same.
            self._dictionary[type_name.lower()] = data
        else:
            logging.error("Invalid key specified: %s", type_name)
            sys.exit(1)

    def add_entry_string(self, key, value):
        # It is valid for a value to be None in which case it means delete
        if value is not None:
            value = str(value)
            # MMCONFIG requires NULL terminator to be explicitly added to string data
            self._add_entry(key, value.encode("utf-8") + b"\0")
            logging.debug(f"Added string {key}")
        else:
            # Delete the entry from the configstore if it exists (no-op if it does
            # not exist).
            self.delete_entry(key, ignore_no_exist=True)
            logging.debug(f"Deleted entry for string {key}")

    def add_entry_hex_string(self, key, hex_string):
        try:
            value = bytes.fromhex(hex_string)
        except (ValueError, TypeError):
            logging.error(f"Invalid hex-string value given for key: {key}. "
                          'Hex strings specified in JSON should be surrounded by quotes (").')
            raise

        self._add_entry(key, value)

        logging.debug(f"Added bytes {key}")

    def add_entry_file(self, key, file):
        with open(file, "rb") as f:
            self._add_entry(key, f.read())

        logging.debug(f"Added file {file}")

    def delete_entry(self, type_name, ignore_no_exist=False):
        if type_name.lower() in self._dictionary:
            del self._dictionary[type_name.lower()]
        elif not ignore_no_exist:
            logging.error("Key not found")
            sys.exit(1)

    def read_entry(self, type_name):
        if type_name.lower() in self._dictionary:
            return self._dictionary[type_name.lower()]
        else:
            logging.error("Key not found")
            sys.exit(1)

    def match(self, other):
        """
        Compares the contents (not version numbers) of the two partitions and returns True
        if they match, else False.
        """
        if other is None:
            return False
        return self._dictionary == other._dictionary

    def dump(self):
        print(f"  Version {self._version}")
        print()
        print(f'  {"Key":{MAX_KEY_LEN}s} | Value')
        print(f'  {"-"*(MAX_KEY_LEN+1)}|--------')
        for key, value in self._dictionary.items():
            try:
                value = value.decode("utf-8")
            except UnicodeDecodeError:
                value = "[hex] " + value.hex()
                if len(value) > 55:
                    value = value[:55] + " ..."
            print(f"  {key:{MAX_KEY_LEN}s} | {value}")


def _main():
    parser = argparse.ArgumentParser(formatter_class=argparse.ArgumentDefaultsHelpFormatter,
                                     description=__doc__)

    parser.add_argument("-o", "--output-file",
                        help="Output to a file instead of stdout")
    parser.add_argument("-d", "--debug", action="store_true",
                        help="Do not do normal output, but print info on stdout")
    parser.add_argument("json", help="JSON to use for config partition (or - for stdin)")

    args = parser.parse_args()

    csp = ConfigStorePartition()
    try:
        json_text = sys.stdin.read() if args.json == "-" else args.json
        json_filename = args.json if args.json == "-" else None
        csp.load_data(json.loads(json_text), json_filename=json_filename)
    except json.JSONDecodeError as e:
        logging.error(e)
        sys.exit(1)
    if args.debug:
        csp.dump()
    elif args.output_file is not None:
        with open(args.output_file, "wb") as f:
            csp.serialize(f)
    else:
        csp.serialize(sys.stdout.buffer)


if __name__ == "__main__":
    _main()
