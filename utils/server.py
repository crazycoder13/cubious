#
# Server script from craft 
# (C) Michael Fogleman, 2013
#

from contextlib import contextmanager
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker
import SocketServer

HOST = '127.0.0.1'
PORT = 4000
RECV_SIZE = 1024
CHUNK_SIZE = 32
ENGINE = 'sqlite:///map.db'

PLAYER = 'U'
BLOCK = 'B'
CHUNK = 'C'
POSITION = 'P'

Session = sessionmaker(bind=create_engine(ENGINE))

@contextmanager
def session():
	session = Session()
	try:
		yield session
		session.commit()
	except:
		session.rollback()
		raise
	finally:
		session.close()

class Server(SocketServer.ThreadingMixIn, SocketServer.TCPServer):
	daemon_threads = True

class Handler(SocketServer.BaseRequestHandler):
	def handle(self):
		model = self.server.model
		model.on_connect(self)
		buf = []
		while True:
			data = self.request.recv(RECV_SIZE)
			if not data:
				break
			buf.extend(data.replace('\r', ''))
			while '\n' in buf:
				index = buf.index('\n')
				line = ''.join(buf[:index])
				buf = buf[index + 1:]
				model.on_data(self, line)
		model.on_disconnect(self)
	def send(self, *args):
		data = ','.join(str(x) for x in args)
		print 'SEND', self.client_id, data
		data = '%s\n' % data
		self.request.sendall(data)

class Model(object):
	def __init__(self):
		self.next_client_id = 0
		self.clients = []
		self.commands = {
			CHUNK: self.on_chunk,
			BLOCK: self.on_block,
			POSITION: self.on_position,
		}
	def on_connect(self, client):
		client.client_id = self.next_client_id
		client.position = (0, 0, 0)
		self.next_client_id += 1
		self.clients.append(client)
		client.send(PLAYER, client.client_id, *client.position)
		self.send_position(client)
		self.send_positions(client)
	def on_data(self, client, data):
		print 'RECV', client.client_id, data
		args = data.split(',')
		command, args = args[0], args[1:]
		if command in self.commands:
			func = self.commands[command]
			func(client, *args)
	def on_disconnect(self, client):
		self.clients.remove(client)
	def on_chunk(self, client, p, q):
		with session() as sql:
			query = 'select x, y, z, w from block where p = :p and q = :q;'
			rows = sql.execute(query, dict(p=p, q=q))
			for x, y, z, w in rows:
				client.send(BLOCK, p, q, x, y, z, w)
	def on_block(self, client, p, q, x, y, z, w):
		with session() as sql:
			query = (
				'insert or replace into block (p, q, x, y, z, w) '
				'values (:p, :q, :x, :y, :z, :w);'
			)
			sql.execute(query, dict(p=p, q=q, x=x, y=y, z=z, w=w))
		self.send_block(client, p, q, x, y, z, w)
	def on_position(self, client, x, y, z):
		client.position = (x, y, z)
		self.send_position(client)
	def send_positions(self, client):
		for other in self.clients:
			if other == client:
				continue
			client.send(POSITION, other.client_id, *other.position)
	def send_position(self, client):
		for other in self.clients:
			if other == client:
				continue
			other.send(POSITION, client.client_id, *client.position)
	def send_block(self, client, p, q, x, y, z, w):
		for other in self.clients:
			if other == client:
				continue
			other.send(BLOCK, p, q, x, y, z, w)

def main():
	queries = [
		'create table if not exists block ('
		'    p int not null,'
		'    q int not null,'
		'    x int not null,'
		'    y int not null,'
		'    z int not null,'
		'    w int not null'
		');',
		'create index if not exists block_pq_idx on block(p, q);',
		'create index if not exists block_xyz_idx on block (x, y, z);',
		'create unique index if not exists block_pqxyz_idx on '
		'    block (p, q, x, y, z);',
	]
	with session() as sql:
		for query in queries:
			sql.execute(query)
			
	print "Starting server!"
	server = Server((HOST, PORT), Handler)
	server.model = Model()
	server.serve_forever()

if __name__ == '__main__':
    main()
