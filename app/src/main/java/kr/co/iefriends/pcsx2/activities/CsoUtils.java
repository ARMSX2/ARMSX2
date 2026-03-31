package kr.co.iefriends.pcsx2.activities;

import android.content.ContentResolver;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import androidx.annotation.Nullable;

import java.io.Closeable;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.util.Arrays;
import java.util.zip.Inflater;

final class CsoUtils {
    private static final int MAGIC_CISO = 0x4F534943;
    private static final int MAGIC_ZISO = 0x4F53495A;

    private CsoUtils() {}

    @Nullable
    static byte[] readRange(ContentResolver cr, Uri uri, long offset, int size) {
        CsoReader reader = null;
        try {
            reader = CsoReader.open(cr, uri);
            if (reader == null) {
                return null;
            }
            return reader.readRange(offset, size);
        } catch (Exception ignored) {
            return null;
        } finally {
            closeQuietly(reader);
        }
    }

    @Nullable
    static InputStream openInputStream(ContentResolver cr, Uri uri) throws IOException {
        CsoReader reader = CsoReader.open(cr, uri);
        if (reader == null) {
            return cr.openInputStream(uri);
        }
        return new CsoInputStream(reader);
    }

    private static void closeQuietly(@Nullable Closeable closeable) {
        if (closeable == null) {
            return;
        }
        try {
            closeable.close();
        } catch (Exception ignored) {}
    }

    static final class CsoReader implements Closeable {
        private final ParcelFileDescriptor descriptor;
        private final FileInputStream inputStream;
        private final FileChannel channel;
        private final long uncompressedSize;
        private final int blockSize;
        private final int alignShift;
        private final int[] indexTable;
        private final int blockCount;

        private CsoReader(ParcelFileDescriptor descriptor, FileInputStream inputStream, FileChannel channel,
                          long uncompressedSize, int blockSize, int alignShift, int[] indexTable) {
            this.descriptor = descriptor;
            this.inputStream = inputStream;
            this.channel = channel;
            this.uncompressedSize = uncompressedSize;
            this.blockSize = blockSize;
            this.alignShift = alignShift;
            this.indexTable = indexTable;
            this.blockCount = indexTable.length - 1;
        }

        static CsoReader open(ContentResolver cr, Uri uri) throws IOException {
            ParcelFileDescriptor pfd = cr.openFileDescriptor(uri, "r");
            if (pfd == null) {
                return null;
            }
            FileInputStream fis = null;
            try {
                fis = new FileInputStream(pfd.getFileDescriptor());
                FileChannel channel = fis.getChannel();
                ByteBuffer header = ByteBuffer.allocate(0x18).order(ByteOrder.LITTLE_ENDIAN);
                if (channel.read(header) < 0x18) {
                    closeQuietly(fis);
                    closeQuietly(pfd);
                    return null;
                }
                header.flip();
                int magic = header.getInt();
                if (magic != MAGIC_CISO && magic != MAGIC_ZISO) {
                    closeQuietly(fis);
                    closeQuietly(pfd);
                    return null;
                }
                int headerSize = header.getInt();
                long uncompressedSize = header.getLong();
                int blockSize = header.getInt();
                header.get();
                int align = header.get() & 0xFF;
                header.get();
                header.get();
                if (blockSize <= 0 || uncompressedSize <= 0 || headerSize < 0x18) {
                    closeQuietly(fis);
                    closeQuietly(pfd);
                    return null;
                }
                int entryCount = (headerSize - 0x18) / 4;
                if (entryCount <= 1) {
                    closeQuietly(fis);
                    closeQuietly(pfd);
                    return null;
                }
                int[] table = new int[entryCount];
                ByteBuffer indexBuffer = ByteBuffer.allocate(entryCount * 4).order(ByteOrder.LITTLE_ENDIAN);
                if (channel.read(indexBuffer) < entryCount * 4) {
                    closeQuietly(fis);
                    closeQuietly(pfd);
                    return null;
                }
                indexBuffer.flip();
                for (int i = 0; i < entryCount; i++) {
                    table[i] = indexBuffer.getInt();
                }
                return new CsoReader(pfd, fis, channel, uncompressedSize, blockSize, align, table);
            } catch (Exception e) {
                closeQuietly(fis);
                closeQuietly(pfd);
                throw e;
            }
        }

        byte[] readRange(long offset, int size) throws IOException {
            if (size <= 0 || offset < 0 || offset >= uncompressedSize) {
                return null;
            }
            int cappedSize = (int) Math.min(size, uncompressedSize - offset);
            byte[] output = new byte[cappedSize];
            byte[] blockBuffer = new byte[blockSize];
            int startBlock = (int) (offset / blockSize);
            int endBlock = Math.min(blockCount, (int) Math.ceil((offset + cappedSize) / (double) blockSize));
            int outOffset = 0;
            int offsetInBlock = (int) (offset % blockSize);
            long remaining = cappedSize;
            for (int block = startBlock; block < endBlock && remaining > 0; block++) {
                int produced = readBlockInto(block, blockBuffer);
                if (produced <= 0) {
                    break;
                }
                int start = (block == startBlock) ? offsetInBlock : 0;
                if (start >= produced) {
                    continue;
                }
                int copyLength = (int) Math.min(produced - start, remaining);
                System.arraycopy(blockBuffer, start, output, outOffset, copyLength);
                outOffset += copyLength;
                remaining -= copyLength;
            }
            if (outOffset == 0) {
                return null;
            }
            if (outOffset < output.length) {
                return Arrays.copyOf(output, outOffset);
            }
            return output;
        }

        int readBlockInto(int blockIndex, byte[] dest) throws IOException {
            if (blockIndex < 0 || blockIndex >= blockCount) {
                return -1;
            }
            long startOffset = (long) (indexTable[blockIndex] & 0x7FFFFFFFL) << alignShift;
            long endOffset = (long) (indexTable[blockIndex + 1] & 0x7FFFFFFFL) << alignShift;
            boolean isPlain = (indexTable[blockIndex] & 0x80000000) != 0;
            int compressedSize = (int) Math.max(0, endOffset - startOffset);
            int expectedSize = (int) Math.min(blockSize, uncompressedSize - ((long) blockIndex * blockSize));
            if (expectedSize <= 0) {
                return 0;
            }
            if (compressedSize == 0) {
                Arrays.fill(dest, 0, expectedSize, (byte) 0);
                return expectedSize;
            }
            byte[] compressed = new byte[compressedSize];
            ByteBuffer buffer = ByteBuffer.wrap(compressed);
            channel.position(startOffset);
            int readTotal = 0;
            while (buffer.hasRemaining()) {
                int r = channel.read(buffer);
                if (r <= 0) {
                    break;
                }
                readTotal += r;
            }
            if (readTotal != compressedSize) {
                return -1;
            }
            if (isPlain) {
                int toCopy = Math.min(expectedSize, compressedSize);
                System.arraycopy(compressed, 0, dest, 0, toCopy);
                if (toCopy < expectedSize) {
                    Arrays.fill(dest, toCopy, expectedSize, (byte) 0);
                }
                return expectedSize;
            }
            Inflater inflater = new Inflater(true);
            try {
                inflater.setInput(compressed);
                int total = 0;
                while (!inflater.finished() && total < expectedSize) {
                    int r = inflater.inflate(dest, total, expectedSize - total);
                    if (r <= 0) {
                        if (inflater.needsInput() || inflater.finished()) {
                            break;
                        }
                    } else {
                        total += r;
                    }
                }
                if (total <= 0) {
                    Arrays.fill(dest, 0, expectedSize, (byte) 0);
                    return expectedSize;
                }
                return total;
            } catch (Exception ignored) {
                return -1;
            } finally {
                inflater.end();
            }
        }

        int getBlockCount() {
            return blockCount;
        }

        int getBlockSize() {
            return blockSize;
        }

        long getUncompressedSize() {
            return uncompressedSize;
        }

        @Override
        public void close() throws IOException {
            try {
                channel.close();
            } finally {
                try {
                    inputStream.close();
                } finally {
                    descriptor.close();
                }
            }
        }
    }

    static final class CsoInputStream extends InputStream {
        private final CsoReader reader;
        private final byte[] blockBuffer;
        private int currentBlock = 0;
        private int blockPos = 0;
        private int blockLimit = 0;
        private long bytesRemaining;

        CsoInputStream(CsoReader reader) {
            this.reader = reader;
            this.blockBuffer = new byte[reader.getBlockSize()];
            this.bytesRemaining = reader.getUncompressedSize();
        }

        @Override
        public int read() throws IOException {
            byte[] single = new byte[1];
            int r = read(single, 0, 1);
            if (r <= 0) {
                return -1;
            }
            return single[0] & 0xFF;
        }

        @Override
        public int read(byte[] b, int off, int len) throws IOException {
            if (b == null) {
                throw new NullPointerException();
            }
            if (off < 0 || len < 0 || len > b.length - off) {
                throw new IndexOutOfBoundsException();
            }
            if (len == 0) {
                return 0;
            }
            if (bytesRemaining <= 0) {
                return -1;
            }
            int total = 0;
            while (len > 0 && bytesRemaining > 0) {
                if (blockPos >= blockLimit) {
                    if (currentBlock >= reader.getBlockCount()) {
                        break;
                    }
                    blockLimit = reader.readBlockInto(currentBlock, blockBuffer);
                    currentBlock++;
                    blockPos = 0;
                    if (blockLimit <= 0) {
                        break;
                    }
                }
                int available = blockLimit - blockPos;
                int copy = Math.min(len, available);
                copy = (int) Math.min(copy, bytesRemaining);
                if (copy <= 0) {
                    break;
                }
                System.arraycopy(blockBuffer, blockPos, b, off, copy);
                off += copy;
                len -= copy;
                total += copy;
                blockPos += copy;
                bytesRemaining -= copy;
            }
            return total > 0 ? total : -1;
        }

        @Override
        public void close() throws IOException {
            reader.close();
        }
    }
}
