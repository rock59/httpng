local http = require 'httpng'

http.cfg{
    threads = 4,
    handler = function(req, io)
        print(req.headers['content-type'])
        print(req.headers['content-length'])
        print('user-agent: ', req.headers['user-agent'])

        return {
            status = 200,
            headers = {
                ['x-req-id'] = 'qwe',
            },
            body = 'content'
        }

    end
}
